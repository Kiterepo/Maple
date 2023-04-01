#include "pch.h"
#include "VpnPlugin.h"
#include "winrt/Windows.Storage.h"
#include "winrt/Windows.Storage.Streams.h"
#include "winrt/Windows.Storage.AccessCache.h"

extern "C" void* lwip_strerr(uint8_t) {
    return (void*)(const char*)"";
}


namespace winrt::Maple_Task::implementation
{
    using Windows::Networking::HostName;
    using Windows::Networking::Sockets::DatagramSocket;
    using Windows::Storage::Streams::IOutputStream;
    using namespace Windows::Networking::Vpn;
    using namespace Windows::Storage;
    using namespace Windows::Storage::AccessCache;

    extern "C" {
        typedef void(__cdecl* netstack_cb)(uint8_t*, size_t, void*);
        void cb(uint8_t* data, size_t size, void* outputStreamAbi) {
            bool needSendDummyBuffer = false;
            {
                std::lock_guard _guard{ VpnPluginInstance->m_decapQueueLock };
                auto& q = VpnPluginInstance->m_decapQueue;
                {
                    std::vector<uint8_t> buf(size);
                    if (memcpy_s(buf.data(), buf.capacity(), data, size)) {
                        return;
                    }
                    needSendDummyBuffer = q.empty();
                    q.emplace(buf);
                }
            }
            if (!needSendDummyBuffer) {
                return;
            }
            IOutputStream outputStream{ nullptr };
            winrt::attach_abi(outputStream, outputStreamAbi);
            try {
                const auto _ = outputStream.WriteAsync(dummyBuffer);
            }
            catch (...) {}
            winrt::detach_abi(outputStream);
        }
    }
    void VpnPlugin::Connect(VpnChannel const& channel)
    {
        try
        {
            ConnectCore(channel);
        }
        catch (std::exception const& ex)
        {
            channel.TerminateConnection(to_hstring(ex.what()));
        }
    }

    void VpnPlugin::ConnectCore(VpnChannel const& channel)
    {
        const auto localhost = HostName{ L"127.0.0.1" };
        DatagramSocket transport{}, backTransport{};
        channel.AssociateTransport(transport, nullptr);
        transport.BindEndpointAsync(localhost, L"").get();
        backTransport.BindEndpointAsync(localhost, L"").get();
        transport.ConnectAsync(localhost, backTransport.Information().LocalPort()).get();
        backTransport.ConnectAsync(localhost, transport.Information().LocalPort()).get();

        VpnRouteAssignment routeScope{};
        routeScope.ExcludeLocalSubnets(true);
        routeScope.Ipv4InclusionRoutes(std::vector{
            // 直接写 0.0.0.0/0 哪怕绑了接口也会绕回环
            // VpnRoute(HostName{ L"0.0.0.0" }, 0)
            VpnRoute(HostName{ L"0.0.0.0" }, 1),
                VpnRoute(HostName{ L"128.0.0.0" }, 1),
            });
        routeScope.Ipv6InclusionRoutes(std::vector{
            VpnRoute(HostName{L"::"}, 1),
            VpnRoute(HostName{L"8000::"}, 1)
            });
        // 排除代理服务器的话就会 os 10023 以一种访问权限不允许的方式做了一个访问套接字的尝试
        // routeScope.Ipv4ExclusionRoutes(std::vector<VpnRoute>{
        //     VpnRoute(HostName{ L"172.25.0.0" }, 16)
        // });

        const auto outputStreamAbi = winrt::detach_abi(backTransport.OutputStream());
        StopLeaf();
        {
            std::lock_guard _guard{ m_decapQueueLock };
            while (!m_decapQueue.empty()) {
                m_decapQueue.pop();
            }
        }
        m_backTransport = backTransport;

        m_netStackHandle = netstack_register(cb, outputStreamAbi);
        if (m_netStackHandle == nullptr) {
            channel.TerminateConnection(L"Error initializing Leaf netstack.");
            return;
        }

        const auto& appData = ApplicationData::Current();
        const auto& configFolderPath = appData.LocalFolder().CreateFolderAsync(L"config", CreationCollisionOption::OpenIfExists).get().Path();
        const auto& localProperties = appData.LocalSettings().Values();
        const auto& outNetif = localProperties.TryLookup(NETIF_SETTING_KEY).try_as<hstring>().value_or(L"");
        const auto& cacheDir = appData.LocalCacheFolder().Path();
        if (
            !SetEnvironmentVariable(L"ASSET_LOCATION", configFolderPath.data())
            || !SetEnvironmentVariable(L"LOG_NO_COLOR", L"true")
            || !SetEnvironmentVariable(L"OUTBOUND_INTERFACE", outNetif.data())
            || !SetEnvironmentVariable(L"CACHE_LOCATION", cacheDir.data())
            || !SetEnvironmentVariable(L"ENABLE_IPV6", L"true")
            ) {
            channel.TerminateConnection(L"Failed to set environment variables: " + winrt::to_hstring((uint32_t)GetLastError()));
            return;
        }

        const auto confPathW = localProperties.TryLookup(CONFIG_PATH_SETTING_KEY).try_as<hstring>().value_or(L"");
        const auto confPath = winrt::to_string(confPathW);
        StorageFolder folder{ nullptr };
        try {
            folder = StorageApplicationPermissions::FutureAccessList().GetFolderAsync(ConfigFolderAccessListKey).get();
        }
        catch (hresult_invalid_argument const&) {}
        thread_local std::vector<HostName> dnsHosts{};
        if (folder)
        {
            if (auto const pos = confPath.rfind('\\'); pos != std::string::npos)
            {
                auto const file = folder.GetFileAsync(to_hstring(confPath.substr(pos + 1))).get();
                auto const text = FileIO::ReadBufferAsync(file).get();
                auto const len = text.Length();
                uint8_t* textPtr{};
                check_hresult(text.as<IBufferByteAccess>()->Buffer(&textPtr));
                m_leaf = uwp_run_leaf_with_config_content(reinterpret_cast<const char*>(textPtr), len,
                    [](const char* dns) {
                        dnsHosts.push_back(HostName{ to_hstring(dns) });
                    });
            }
            else
            {
                channel.TerminateConnection(L"Config folder has been changed. Please reset the default configuration file.");
                return;
            }
        }
        else
        {
            m_leaf = uwp_run_leaf(confPath.data(), [](const char* dns) {
                dnsHosts.push_back(HostName{ to_hstring(dns) });
                });
        }

        if (m_leaf == nullptr) {
            channel.TerminateConnection(L"Error initializing Leaf runtime.\r\nPlease check your configuration file and default interface.\r\nPlease make sure all associated files (.dat, .mmdb, .cer) exist.");
            StopLeaf();
            return;
        }
        VpnDomainNameAssignment dnsAssignment{};
        dnsAssignment.DomainNameList().Append(VpnDomainNameInfo(
            L".",
            VpnDomainNameType::Suffix,
            single_threaded_vector(std::move(dnsHosts)),
            single_threaded_vector(std::vector<HostName>())));

        channel.StartWithMainTransport(
            std::vector{ HostName{ L"192.168.3.1" } },
            std::vector{ HostName{L"fd00::2"} },
            nullptr,
            routeScope,
            dnsAssignment,
            1500,
            1512,
            false,
            transport
        );
    }
    void VpnPlugin::StopLeaf() {
        m_backTransport = nullptr;

        auto leafHandle = m_leaf;
        if (leafHandle != nullptr) {
            uwp_stop_leaf(leafHandle);
            m_leaf = nullptr;
        }

        auto netStackHandle = m_netStackHandle;
        if (netStackHandle != nullptr) {
            const auto context = netstack_release(netStackHandle);
            m_netStackHandle = nullptr;
            // Release context, which is an ABI of IOutputStream
            IInspectable obj{};
            winrt::attach_abi(obj, context);
            m_netStackHandle = nullptr;
        }
    }
    void VpnPlugin::Disconnect(VpnChannel const& channel)
    {
        try {
            channel.Stop();
        }
        catch (...) {}
        StopLeaf();
    }
    void VpnPlugin::GetKeepAlivePayload(VpnChannel const&, VpnPacketBuffer&)
    {
    }
    void VpnPlugin::Encapsulate([[maybe_unused]] VpnChannel const& channel, VpnPacketBufferList const& packets, VpnPacketBufferList const&)
    {
        auto packetCount = packets.Size();
        while (packetCount-- > 0) {
            const auto packet = packets.RemoveAtBegin();
            const auto buffer = packet.Buffer();
            netstack_send(m_netStackHandle, buffer.data(), static_cast<size_t>(buffer.Length()));
            packets.Append(packet);
        }
    }
    void VpnPlugin::Decapsulate(VpnChannel const& channel, [[maybe_unused]] VpnPacketBuffer const& encapBuffer, VpnPacketBufferList const& decapsulatedPackets, VpnPacketBufferList const&)
    {
        std::lock_guard _guard{ VpnPluginInstance->m_decapQueueLock };
        auto& q = VpnPluginInstance->m_decapQueue;
        while (!q.empty()) {
            auto&& incomingBuffer = q.front();
            const auto outBuffer = channel.GetVpnReceivePacketBuffer();
            decapsulatedPackets.Append(outBuffer);
            const auto outBuf = outBuffer.Buffer();
            const auto size = incomingBuffer.size();
            if (memcpy_s(outBuf.data(), outBuf.Capacity(), incomingBuffer.data(), size)) {
                return;
            }
            outBuf.Length(static_cast<uint32_t>(size));
            q.pop();
        }
    }
}
