#include "xvr/UdpFrameSink.h"

#include "xvr/Log.h"
#include "xvr/Protocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>

// After winsock2.h deliberately: <thread> can pull in windows.h transitively, and if that
// happens first it drags in the original winsock.h and every socket type is defined twice.
#include <chrono>
#include <cstring>
#include <thread>

namespace xvr {
namespace {

// The largest frame we are willing to fragment. 65535 fragments is the wire limit, but a
// frame anywhere near that means something upstream is badly wrong, and blindly sending it
// would flood the link.
constexpr size_t kMaxFrameBytes = 4 * 1024 * 1024;

} // namespace

UdpFrameSink::~UdpFrameSink()
{
    Shutdown();
}

void UdpFrameSink::InitializeUnbound(uint8_t eye)
{
    Initialize(std::string(), 0, eye);
}

void UdpFrameSink::SetDestination(const unsigned char* address, int addressSize)
{
    if (addressSize > 0 && addressSize <= static_cast<int>(sizeof(m_destination)))
    {
        std::memcpy(m_destination, address, static_cast<size_t>(addressSize));
        m_destinationSize = addressSize;
    }
}

void UdpFrameSink::Initialize(const std::string& destinationHost, uint16_t destinationPort,
                              uint8_t eye)
{
    m_eye = eye;

    WSADATA wsaData{};
    const int startupResult = ::WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (startupResult != 0)
    {
        LogError(L"WSAStartup failed: {}", startupResult);
        return;
    }
    m_winsockStarted = true;

    const SOCKET handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == INVALID_SOCKET)
    {
        LogError(L"UDP socket creation failed: {}", ::WSAGetLastError());
        return;
    }
    m_socket = static_cast<uintptr_t>(handle);

    // A send buffer large enough for a burst of fragments. Without this the stack can
    // block or drop when a keyframe - several times a normal frame - goes out at once.
    int sendBufferBytes = 1 << 20;
    ::setsockopt(handle, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBufferBytes),
                 sizeof(sendBufferBytes));

    m_packetBuffer.resize(sizeof(net::VideoFragmentHeader) + net::kFragmentPayloadBytes);

    // An empty host means "no destination yet" - the caller will supply one when a client
    // announces itself. Frames are silently discarded until then.
    if (destinationHost.empty())
    {
        LogInfo(L"UDP sink for eye {} ready, awaiting client", eye);
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(destinationPort);
    if (::inet_pton(AF_INET, destinationHost.c_str(), &address.sin_addr) != 1)
    {
        LogError(L"Could not parse destination address");
        return;
    }

    std::memcpy(m_destination, &address, sizeof(address));
    m_destinationSize = sizeof(address);

    LogInfo(L"UDP sink for eye {} targeting port {}", eye, destinationPort);
}

void UdpFrameSink::Shutdown()
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

bool UdpFrameSink::SendDatagram(const void* data, size_t size)
{
    const int sent = ::sendto(static_cast<SOCKET>(m_socket), static_cast<const char*>(data),
                              static_cast<int>(size), 0,
                              reinterpret_cast<const sockaddr*>(m_destination), m_destinationSize);
    return sent == static_cast<int>(size);
}

void UdpFrameSink::OnEncodedFrame(const EncodedFrame& frame)
{
    if (m_socket == ~static_cast<uintptr_t>(0) || m_destinationSize == 0 || frame.size == 0)
    {
        return;
    }

    if (frame.size > kMaxFrameBytes)
    {
        ++m_stats.oversizeFrames;
        LogWarn(L"Dropping absurd frame of {} bytes", frame.size);
        return;
    }

    const size_t payloadSize = net::kFragmentPayloadBytes;
    const auto fragmentCount =
        static_cast<uint16_t>((frame.size + payloadSize - 1) / payloadSize);

    // Pacing. A frame's fragments are spread across part of the frame interval rather than
    // emitted as fast as sendto returns.
    //
    // A still scene compresses small and this changes nothing. A moving one does not: the
    // frame grows several times over, and the burst that used to fit in the receiver's
    // buffer stops fitting - which is exactly why the picture breaks up while moving and
    // looks fine standing still. Both eyes finishing at the same moment doubles it again.
    //
    // Half the interval is the budget, so a frame is always fully sent well before the next
    // one is ready, and the added latency is bounded by construction. Below the threshold
    // the burst is small enough not to matter and pacing would only add delay.
    constexpr uint16_t kPacingThreshold = 12;
    const bool pace = m_frameIntervalUs > 0 && fragmentCount > kPacingThreshold;
    const auto sendStart = std::chrono::steady_clock::now();
    const double budgetUs = pace ? m_frameIntervalUs * 0.5 : 0.0;

    for (uint16_t index = 0; index < fragmentCount; ++index)
    {
        if (pace && index > 0)
        {
            // Where this fragment should go out, measured from the first one.
            const double dueUs = budgetUs * index / fragmentCount;
            for (;;)
            {
                const double elapsedUs =
                    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() -
                                                             sendStart)
                        .count();
                if (elapsedUs >= dueUs)
                {
                    break;
                }
                // Sleeping is not usable at this granularity - the default Windows timer
                // resolution is coarser than a whole frame, so a requested 200 us wait can
                // cost 15 ms. Yielding keeps the wait short without pinning the core.
                std::this_thread::yield();
            }
        }

        const size_t offset = static_cast<size_t>(index) * payloadSize;
        const size_t thisPayload = (std::min)(payloadSize, frame.size - offset);

        net::VideoFragmentHeader header;
        header.common.type = static_cast<uint8_t>(net::PacketType::VideoFragment);
        header.eye = frame.eye;
        header.flags = static_cast<uint8_t>(
            (frame.keyframe ? net::VideoFlag_Keyframe : 0) |
            (index + 1 == fragmentCount ? net::VideoFlag_FinalFragment : 0));
        header.fragmentIndex = index;
        header.fragmentCount = fragmentCount;
        header.frameIndex = frame.frameIndex;
        header.captureTimeUs = frame.captureTimeUs;
        header.renderPoseSequence = frame.poseSequence;

        std::memcpy(m_packetBuffer.data(), &header, sizeof(header));
        std::memcpy(m_packetBuffer.data() + sizeof(header), frame.data + offset, thisPayload);

        if (SendDatagram(m_packetBuffer.data(), sizeof(header) + thisPayload))
        {
            ++m_stats.fragmentsSent;
            m_stats.bytesSent += sizeof(header) + thisPayload;
        }
        else
        {
            // Deliberately not retried. A retry would arrive later than the frame is
            // useful for, and would delay every fragment queued behind it.
            ++m_stats.sendFailures;
        }
    }

    ++m_stats.framesSent;
}

} // namespace xvr
