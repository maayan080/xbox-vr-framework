// Winsock 2 ahead of everything else. ClientLink.h reaches d3d11.h and so windows.h, which
// pulls in winsock 1 unless WIN32_LEAN_AND_MEAN is set - and then every socket type is
// defined twice. UWP projects happen to set it, plain desktop ones do not, so the order is
// enforced here rather than left to whoever is compiling this.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "xvr/ClientLink.h"

#include "xvr/Log.h"
#include "xvr/Protocol.h"

#include <cstring>

namespace xvr {

ClientLink::~ClientLink()
{
    Stop();
}

bool ClientLink::Start(uint16_t discoveryPort)
{
    WSADATA wsaData{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        LogError(L"ClientLink: WSAStartup failed");
        return false;
    }
    m_winsockStarted = true;

    const SOCKET handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == INVALID_SOCKET)
    {
        LogError(L"ClientLink: socket creation failed: {}", ::WSAGetLastError());
        return false;
    }

    // Non-blocking: discovery is polled from the render loop and must never stall a frame.
    u_long nonBlocking = 1;
    ::ioctlsocket(handle, FIONBIO, &nonBlocking);

    BOOL broadcast = TRUE;
    ::setsockopt(handle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast),
                 sizeof(broadcast));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = ::htons(discoveryPort);

    if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        LogError(L"ClientLink: could not bind discovery port {}: {}", discoveryPort,
                 ::WSAGetLastError());
        ::closesocket(handle);
        return false;
    }

    m_socket = static_cast<uintptr_t>(handle);
    LogInfo(L"ClientLink listening for clients on UDP {}", discoveryPort);
    return true;
}

void ClientLink::Stop()
{
    if (m_socket != ~static_cast<uintptr_t>(0))
    {
        ::closesocket(static_cast<SOCKET>(m_socket));
        m_socket = ~static_cast<uintptr_t>(0);
    }

    if (m_winsockStarted)
    {
        ::WSACleanup();
        m_winsockStarted = false;
    }
}

void ClientLink::DrainSocket(const FrameConfig& config)
{
    if (m_socket == ~static_cast<uintptr_t>(0))
    {
        return;
    }

    unsigned char buffer[512];

    while (true)
    {
        sockaddr_in from{};
        int fromSize = sizeof(from);
        const int received =
            ::recvfrom(static_cast<SOCKET>(m_socket), reinterpret_cast<char*>(buffer),
                       sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromSize);

        if (received < static_cast<int>(sizeof(net::CommonHeader)))
        {
            return;
        }

        net::CommonHeader header{};
        std::memcpy(&header, buffer, sizeof(header));

        if (!net::IsValidHeader(header))
        {
            // A packet that is ours but the wrong version is worth saying so about, once.
            // Silently dropping it is indistinguishable from the client never sending
            // anything, which turns a one-line version mismatch into a network hunt.
            if (header.magic == net::kMagic && !m_warnedVersionMismatch)
            {
                m_warnedVersionMismatch = true;
                LogError(L"Client speaks protocol v{}, host expects v{} - update both sides",
                         header.version, net::kProtocolVersion);
            }
            continue;
        }

        if (header.type == static_cast<uint8_t>(net::PacketType::KeyframeRequest) &&
            received >= static_cast<int>(sizeof(net::KeyframeRequest)))
        {
            // The client could not assemble a frame. Everything it decodes from here
            // references data it never received, so it stays corrupt until an IDR arrives.
            m_keyframeRequested.store(true, std::memory_order_release);
            continue;
        }

        if (header.type == static_cast<uint8_t>(net::PacketType::PoseUpdate) &&
            received >= static_cast<int>(sizeof(net::PoseUpdate)))
        {
            net::PoseUpdate update{};
            std::memcpy(&update, buffer, sizeof(update));

            const auto arrivedAt = std::chrono::steady_clock::now();

            // Wrapping-safe comparison, so a session long enough to overflow the counter
            // does not freeze the pose permanently.
            const int32_t delta = static_cast<int32_t>(update.poseSequence - m_latestSequence);

            // If nothing has been accepted for a while, take whatever arrives and rebase.
            //
            // Without this, a client that restarts begins numbering from 1 again while the
            // host still holds a large sequence - so every pose looks stale and is dropped,
            // permanently, with no way back. The symptom is a pose age that climbs forever
            // while packets keep arriving, and the headset falling back to head-locked.
            const bool linkStale =
                m_havePose && (arrivedAt - m_poseReceivedAt) > std::chrono::milliseconds(500);

            if (m_havePose && delta <= 0 && !linkStale)
            {
                ++m_posesRejected;
                continue;
            }

            if (linkStale && delta <= 0)
            {
                LogWarn(L"Pose sequence went backwards ({} -> {}) after a gap; rebasing",
                        m_latestSequence, update.poseSequence);
            }

            m_latestSequence = update.poseSequence;
            ++m_posePackets;
            m_poseReceivedAt = arrivedAt;

            m_latestPose = {};
            m_latestPose.poseValid = true;
            std::memcpy(m_latestPose.headPosition, update.headPosition, sizeof(float) * 3);
            std::memcpy(m_latestPose.headOrientation, update.headOrientation, sizeof(float) * 4);

            if ((update.flags & net::PoseFlag_HasEyeData) != 0)
            {
                for (int eye = 0; eye < 2; ++eye)
                {
                    std::memcpy(m_latestPose.eyes[eye].position, update.eyes[eye].position,
                                sizeof(float) * 3);
                    std::memcpy(m_latestPose.eyes[eye].orientation, update.eyes[eye].orientation,
                                sizeof(float) * 4);
                    std::memcpy(m_latestPose.eyes[eye].fov, update.eyes[eye].fov,
                                sizeof(float) * 4);
                }
            }
            else
            {
                // No per-eye data: use the head pose for both. Stereo collapses to mono,
                // which is wrong but stable - far better than inventing an IPD.
                for (auto& eye : m_latestPose.eyes)
                {
                    std::memcpy(eye.position, update.headPosition, sizeof(float) * 3);
                    std::memcpy(eye.orientation, update.headOrientation, sizeof(float) * 4);
                }
            }

            for (int hand = 0; hand < 2; ++hand)
            {
                const net::ControllerState& source = update.controllers[hand];
                ControllerInput& target = m_latestPose.controllers[hand];

                target.active = (source.buttons & net::Button_Active) != 0;
                target.buttons = source.buttons;
                target.trigger = source.trigger;
                target.squeeze = source.squeeze;
                target.thumbstick[0] = source.thumbstick[0];
                target.thumbstick[1] = source.thumbstick[1];

                std::memcpy(target.gripPosition, source.gripPosition, sizeof(float) * 3);
                std::memcpy(target.gripOrientation, source.gripOrientation, sizeof(float) * 4);
                std::memcpy(target.aimPosition, source.aimPosition, sizeof(float) * 3);
                std::memcpy(target.aimOrientation, source.aimOrientation, sizeof(float) * 4);
            }

            m_havePose = true;
            continue;
        }

        if (header.type == static_cast<uint8_t>(net::PacketType::Handshake))
        {
            net::Handshake request{};
            if (received >= static_cast<int>(sizeof(request)))
            {
                std::memcpy(&request, buffer, sizeof(request));
            }

            // Reply with what the hardware will actually do, not what was asked for.
            net::HandshakeAck ack{};
            ack.common.type = static_cast<uint8_t>(net::PacketType::HandshakeAck);
            ack.width = static_cast<uint16_t>(config.width);
            ack.height = static_cast<uint16_t>(config.height);
            ack.frameRate = static_cast<uint8_t>(config.frameRate);
            ack.codec = 0;

            ::sendto(static_cast<SOCKET>(m_socket), reinterpret_cast<const char*>(&ack),
                     sizeof(ack), 0, reinterpret_cast<const sockaddr*>(&from), fromSize);

            char text[INET_ADDRSTRLEN]{};
            ::inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));

            m_pendingEndpoint = {};
            std::memcpy(m_pendingEndpoint.address, &from, sizeof(from));
            m_pendingEndpoint.size = fromSize;
            m_pendingEndpoint.text = text;
            m_pendingEndpoint.port = ::ntohs(from.sin_port);
            m_pendingEndpoint.requestedWidth = request.requestedWidth;
            m_pendingEndpoint.requestedHeight = request.requestedHeight;
            m_pendingEndpoint.requestedFrameRate = request.requestedFrameRate;
            m_hasPendingEndpoint = true;

            LogInfo(L"Client discovered at {}:{} (requested {}x{}@{})",
                    std::wstring(m_pendingEndpoint.text.begin(), m_pendingEndpoint.text.end()),
                    m_pendingEndpoint.port, request.requestedWidth, request.requestedHeight,
                    request.requestedFrameRate);
        }
    }
}

bool ClientLink::PollPose(FrameContext& frame)
{
    DrainSocket(FrameConfig{});

    if (!m_havePose)
    {
        return false;
    }

    const double ageMs = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - m_poseReceivedAt)
                             .count();

    // A pose this old is not a pose, it is a stuck value. Rendering from it produces a
    // view frozen wherever the head used to be, while the client - unable to find that
    // ancient sequence in its history - quietly gives up on reprojection. Reporting no
    // pose is both more honest and more diagnosable than rendering from a stale one.
    if (ageMs > 1000.0)
    {
        return false;
    }

    frame.poseValid = true;
    frame.poseAgeMs = ageMs;
    // Echoed back on every frame rendered from this pose, so the client can reproject.
    frame.poseSequence = m_latestSequence;
    std::memcpy(frame.headPosition, m_latestPose.headPosition, sizeof(float) * 3);
    std::memcpy(frame.headOrientation, m_latestPose.headOrientation, sizeof(float) * 4);
    frame.eyes[0] = m_latestPose.eyes[0];
    frame.eyes[1] = m_latestPose.eyes[1];
    frame.controllers[0] = m_latestPose.controllers[0];
    frame.controllers[1] = m_latestPose.controllers[1];

    return true;
}

bool ClientLink::PollForClient(const FrameConfig& config, Endpoint& endpoint)
{
    DrainSocket(config);

    if (!m_hasPendingEndpoint)
    {
        return false;
    }

    endpoint = m_pendingEndpoint;
    m_hasPendingEndpoint = false;
    return true;
}

} // namespace xvr
