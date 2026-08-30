// XVR stream receiver.
//
// A throwaway test target for Phase 1 step 2: proves the wire format works before the
// Quest client exists, so transport can be debugged on its own rather than tangled up
// with OpenXR and hardware decode.
//
// Runs on an ordinary PC on the same network as the Xbox. It:
//   - receives UDP video fragments and reassembles frames per docs/protocol.md
//   - reports loss, reordering, jitter and bitrate
//   - writes a playable .h264 per eye
//   - serves a local page that decodes and displays the stream live in a browser
//
// Deliberately a single file with no dependencies: it has to be a plain .exe someone can
// double-click, not something with a runtime to install.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include "xvr/Protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

// Defined in ViewerHtml.cpp. Declared at global scope deliberately: inside the anonymous
// namespace below it would have internal linkage and never resolve.
extern const char* kViewerHtml;

namespace {

constexpr uint16_t kVideoPort = 9944;
constexpr uint16_t kWebPort = 8080;

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct Stats
{
    std::atomic<uint64_t> fragmentsReceived{ 0 };
    std::atomic<uint64_t> bytesReceived{ 0 };
    std::atomic<uint64_t> framesCompleted{ 0 };
    std::atomic<uint64_t> framesDropped{ 0 };   // incomplete when superseded
    std::atomic<uint64_t> fragmentsLost{ 0 };   // inferred from gaps
    std::atomic<uint64_t> outOfOrder{ 0 };
    std::atomic<uint64_t> keyframes{ 0 };
    std::atomic<int64_t> lastLatencyUs{ 0 };
};

Stats g_stats;

// ---------------------------------------------------------------------------
// Frame reassembly
// ---------------------------------------------------------------------------

struct PartialFrame
{
    std::vector<uint8_t> data;
    std::vector<bool> received;
    uint16_t fragmentCount = 0;
    uint16_t haveCount = 0;
    bool keyframe = false;
    uint64_t captureTimeUs = 0;
    std::chrono::steady_clock::time_point firstSeen;
};

// One eye's worth of reassembly state.
class EyeAssembler
{
public:
    explicit EyeAssembler(uint8_t eye) : m_eye(eye)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "received-eye%u.h264", eye);
        m_file.open(name, std::ios::binary | std::ios::trunc);
    }

    // Returns a complete frame's bytes, or empty if this fragment did not finish one.
    std::vector<uint8_t> Accept(const xvr::net::VideoFragmentHeader& header,
                                const uint8_t* payload, size_t payloadSize)
    {
        // A fragment for a frame we already finished or abandoned is late; ignore it
        // rather than resurrecting a frame whose moment has passed.
        if (header.frameIndex < m_currentFrame && m_currentFrame != 0)
        {
            g_stats.outOfOrder.fetch_add(1);
            return {};
        }

        // A newer frame starting means the old one will never complete. Drop it now -
        // holding it would only add latency, and it can no longer be displayed on time.
        if (header.frameIndex > m_currentFrame && m_partial.fragmentCount != 0)
        {
            const uint16_t missing = m_partial.fragmentCount - m_partial.haveCount;
            g_stats.fragmentsLost.fetch_add(missing);
            g_stats.framesDropped.fetch_add(1);
            m_partial = {};
        }

        if (header.frameIndex != m_currentFrame || m_partial.fragmentCount == 0)
        {
            m_currentFrame = header.frameIndex;
            m_partial = {};
            m_partial.fragmentCount = header.fragmentCount;
            m_partial.received.assign(header.fragmentCount, false);
            m_partial.data.resize(static_cast<size_t>(header.fragmentCount) *
                                  xvr::net::kFragmentPayloadBytes);
            m_partial.keyframe = (header.flags & xvr::net::VideoFlag_Keyframe) != 0;
            m_partial.captureTimeUs = header.captureTimeUs;
            m_partial.firstSeen = std::chrono::steady_clock::now();
        }

        if (header.fragmentIndex >= m_partial.fragmentCount ||
            m_partial.received[header.fragmentIndex])
        {
            return {};
        }

        const size_t offset =
            static_cast<size_t>(header.fragmentIndex) * xvr::net::kFragmentPayloadBytes;
        std::memcpy(m_partial.data.data() + offset, payload, payloadSize);
        m_partial.received[header.fragmentIndex] = true;
        ++m_partial.haveCount;

        // The final fragment is usually short, so the true frame size is only known once
        // it arrives.
        if (header.flags & xvr::net::VideoFlag_FinalFragment)
        {
            m_finalSize = offset + payloadSize;
        }

        if (m_partial.haveCount == m_partial.fragmentCount && m_finalSize > 0)
        {
            std::vector<uint8_t> complete(m_partial.data.begin(),
                                          m_partial.data.begin() + m_finalSize);

            if (m_partial.keyframe)
            {
                g_stats.keyframes.fetch_add(1);
            }
            g_stats.framesCompleted.fetch_add(1);

            if (m_file.is_open())
            {
                m_file.write(reinterpret_cast<const char*>(complete.data()),
                             static_cast<std::streamsize>(complete.size()));
                m_file.flush();
            }

            m_partial = {};
            m_finalSize = 0;
            m_currentFrame = header.frameIndex + 1;
            return complete;
        }

        return {};
    }

private:
    uint8_t m_eye;
    uint32_t m_currentFrame = 0;
    size_t m_finalSize = 0;
    PartialFrame m_partial;
    std::ofstream m_file;
};

// ---------------------------------------------------------------------------
// Browser delivery
// ---------------------------------------------------------------------------

// Completed frames waiting to be pushed to any connected browser. Bounded, and the
// oldest are discarded first: a viewer that cannot keep up should fall behind and skip,
// not make the receiver accumulate memory.
class FrameQueue
{
public:
    void Push(std::vector<uint8_t> frame, uint8_t eye, bool keyframe)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_frames.size() > 8)
        {
            m_frames.erase(m_frames.begin());
        }
        m_frames.push_back({ std::move(frame), eye, keyframe });
    }

    struct Entry
    {
        std::vector<uint8_t> data;
        uint8_t eye;
        bool keyframe;
    };

    bool Pop(Entry& out)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_frames.empty())
        {
            return false;
        }
        out = std::move(m_frames.front());
        m_frames.erase(m_frames.begin());
        return true;
    }

private:
    std::mutex m_mutex;
    std::vector<Entry> m_frames;
};

FrameQueue g_frameQueue;

std::string Base64(const uint8_t* data, size_t size)
{
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < size; i += 3)
    {
        const uint32_t a = data[i];
        const uint32_t b = (i + 1 < size) ? data[i + 1] : 0;
        const uint32_t c = (i + 2 < size) ? data[i + 2] : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;

        out += table[(triple >> 18) & 0x3F];
        out += table[(triple >> 12) & 0x3F];
        out += (i + 1 < size) ? table[(triple >> 6) & 0x3F] : '=';
        out += (i + 2 < size) ? table[triple & 0x3F] : '=';
    }
    return out;
}

std::string Sha1Base64(const std::string& input)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0);

    uint8_t hash[20]{};
    BCRYPT_HASH_HANDLE hashHandle = nullptr;
    BCryptCreateHash(algorithm, &hashHandle, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(hashHandle, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                   static_cast<ULONG>(input.size()), 0);
    BCryptFinishHash(hashHandle, hash, sizeof(hash), 0);
    BCryptDestroyHash(hashHandle);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    return Base64(hash, sizeof(hash));
}

void SendAll(SOCKET client, const char* data, size_t size)
{
    size_t sent = 0;
    while (sent < size)
    {
        const int n = ::send(client, data + sent, static_cast<int>(size - sent), 0);
        if (n <= 0)
        {
            return;
        }
        sent += static_cast<size_t>(n);
    }
}

// Minimal server-to-client WebSocket frame. No masking is required in this direction,
// and we never fragment at the WebSocket level.
void SendWebSocketFrame(SOCKET client, const uint8_t* payload, size_t size)
{
    std::vector<uint8_t> header;
    header.push_back(0x82); // FIN + binary opcode

    if (size < 126)
    {
        header.push_back(static_cast<uint8_t>(size));
    }
    else if (size <= 0xFFFF)
    {
        header.push_back(126);
        header.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
        header.push_back(static_cast<uint8_t>(size & 0xFF));
    }
    else
    {
        header.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            header.push_back(static_cast<uint8_t>((size >> shift) & 0xFF));
        }
    }

    SendAll(client, reinterpret_cast<const char*>(header.data()), header.size());
    SendAll(client, reinterpret_cast<const char*>(payload), size);
}

void HandleWebClient(SOCKET client)
{
    char request[4096]{};
    const int received = ::recv(client, request, sizeof(request) - 1, 0);
    if (received <= 0)
    {
        ::closesocket(client);
        return;
    }

    const std::string text(request, static_cast<size_t>(received));

    const size_t keyPos = text.find("Sec-WebSocket-Key:");
    if (keyPos == std::string::npos)
    {
        // Plain HTTP: serve the viewer page.
        const std::string body = kViewerHtml;
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        SendAll(client, response.data(), response.size());
        ::closesocket(client);
        return;
    }

    size_t keyStart = text.find_first_not_of(" ", keyPos + 18);
    size_t keyEnd = text.find("\r\n", keyStart);
    const std::string key = text.substr(keyStart, keyEnd - keyStart);

    const std::string accept =
        Sha1Base64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

    const std::string handshake = "HTTP/1.1 101 Switching Protocols\r\n"
                                  "Upgrade: websocket\r\n"
                                  "Connection: Upgrade\r\n"
                                  "Sec-WebSocket-Accept: " +
                                  accept + "\r\n\r\n";
    SendAll(client, handshake.data(), handshake.size());

    std::printf("[web] viewer connected\n");

    // Stream frames until the socket dies. Each message is [eye][keyframe][H.264 bytes].
    while (true)
    {
        FrameQueue::Entry entry;
        if (!g_frameQueue.Pop(entry))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        std::vector<uint8_t> message;
        message.reserve(entry.data.size() + 2);
        message.push_back(entry.eye);
        message.push_back(entry.keyframe ? 1 : 0);
        message.insert(message.end(), entry.data.begin(), entry.data.end());

        SendWebSocketFrame(client, message.data(), message.size());

        // Detect a closed socket cheaply.
        char probe[1];
        const int peeked = ::recv(client, probe, 1, MSG_PEEK);
        if (peeked == 0)
        {
            break;
        }
    }

    std::printf("[web] viewer disconnected\n");
    ::closesocket(client);
}

void WebServerThread()
{
    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
    {
        std::printf("[web] could not create socket\n");
        return;
    }

    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = ::htons(kWebPort);

    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 4) != 0)
    {
        std::printf("[web] could not listen on port %u\n", kWebPort);
        return;
    }

    while (true)
    {
        const SOCKET client = ::accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET)
        {
            continue;
        }
        std::thread(HandleWebClient, client).detach();
    }
}

// ---------------------------------------------------------------------------
// Video receive
// ---------------------------------------------------------------------------

void DiscoveryThread(SOCKET videoSocket);

void ReceiveThread()
{
    const SOCKET socketHandle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET)
    {
        std::printf("could not create UDP socket\n");
        return;
    }

    // Generous receive buffer: a keyframe arrives as a burst of fragments, and a small
    // buffer would drop them at the kernel before we ever see them.
    int receiveBuffer = 4 << 20;
    ::setsockopt(socketHandle, SOL_SOCKET, SO_RCVBUF,
                 reinterpret_cast<const char*>(&receiveBuffer), sizeof(receiveBuffer));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = ::htons(kVideoPort);

    if (::bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        std::printf("could not bind UDP port %u\n", kVideoPort);
        return;
    }

    std::printf("listening for video on UDP %u\n", kVideoPort);

    // Announce from this same socket, so the host's reply and its video both land here.
    std::thread(DiscoveryThread, socketHandle).detach();

    EyeAssembler assemblers[2] = { EyeAssembler(0), EyeAssembler(1) };
    std::vector<uint8_t> buffer(2048);

    while (true)
    {
        const int received = ::recvfrom(socketHandle, reinterpret_cast<char*>(buffer.data()),
                                        static_cast<int>(buffer.size()), 0, nullptr, nullptr);
        if (received < static_cast<int>(sizeof(xvr::net::VideoFragmentHeader)))
        {
            continue;
        }

        xvr::net::VideoFragmentHeader header{};
        std::memcpy(&header, buffer.data(), sizeof(header));

        // Anything not ours is dropped before it is parsed further.
        if (!xvr::net::IsValidHeader(header.common))
        {
            continue;
        }

        if (header.common.type == static_cast<uint8_t>(xvr::net::PacketType::HandshakeAck))
        {
            xvr::net::HandshakeAck ack{};
            if (received >= static_cast<int>(sizeof(ack)))
            {
                std::memcpy(&ack, buffer.data(), sizeof(ack));
                std::printf("[discovery] host accepted: %ux%u per eye @%ufps\n", ack.width,
                            ack.height, ack.frameRate);
            }
            continue;
        }

        if (header.common.type != static_cast<uint8_t>(xvr::net::PacketType::VideoFragment) ||
            header.eye > 1)
        {
            continue;
        }

        g_stats.fragmentsReceived.fetch_add(1);
        g_stats.bytesReceived.fetch_add(static_cast<uint64_t>(received));

        const size_t payloadSize = static_cast<size_t>(received) - sizeof(header);
        std::vector<uint8_t> frame = assemblers[header.eye].Accept(
            header, buffer.data() + sizeof(header), payloadSize);

        if (!frame.empty())
        {
            const auto nowUs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());

            // Only meaningful once the clocks are related; until then it is a relative
            // figure useful for spotting drift and stalls, not an absolute latency.
            g_stats.lastLatencyUs.store(static_cast<int64_t>(nowUs - header.captureTimeUs));

            const bool keyframe = (header.flags & xvr::net::VideoFlag_Keyframe) != 0;
            g_frameQueue.Push(std::move(frame), header.eye, keyframe);
        }
    }
}

// Announces this receiver on the local subnet until video starts arriving.
//
// Broadcasting from the same socket the video will arrive on means the host can simply
// reply to the source address, so neither end needs to be told the other's IP - which
// matters because entering one on a console with a controller is painful and error-prone.
void DiscoveryThread(SOCKET videoSocket)
{
    xvr::net::Handshake handshake{};
    handshake.common.type = static_cast<uint8_t>(xvr::net::PacketType::Handshake);
    handshake.requestedWidth = 1920;
    handshake.requestedHeight = 1088;
    handshake.requestedFrameRate = 72;

    BOOL broadcast = TRUE;
    ::setsockopt(videoSocket, SOL_SOCKET, SO_BROADCAST,
                 reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = ::htons(9943);
    target.sin_addr.s_addr = INADDR_BROADCAST;

    bool announced = false;
    while (true)
    {
        // Keep announcing while nothing is arriving: the host may not be running yet, or
        // may have been restarted since we last found it.
        if (g_stats.fragmentsReceived.load() == 0)
        {
            ::sendto(videoSocket, reinterpret_cast<const char*>(&handshake), sizeof(handshake), 0,
                     reinterpret_cast<const sockaddr*>(&target), sizeof(target));

            if (!announced)
            {
                std::printf("[discovery] announcing on the local network, waiting for the "
                            "Xbox...\n");
                announced = true;
            }
        }
        else if (announced)
        {
            std::printf("[discovery] video is arriving; stopping announcements\n");
            announced = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void StatsThread()
{
    uint64_t previousBytes = 0;
    uint64_t previousFrames = 0;

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        const uint64_t bytes = g_stats.bytesReceived.load();
        const uint64_t frames = g_stats.framesCompleted.load();

        const double mbps = static_cast<double>(bytes - previousBytes) * 8.0 / 1'000'000.0;
        const uint64_t fps = frames - previousFrames;

        std::printf("[stats] %5.1f Mbps | %3llu frames/s | completed %llu | dropped %llu | "
                    "lost frags %llu | reordered %llu | keyframes %llu\n",
                    mbps, static_cast<unsigned long long>(fps),
                    static_cast<unsigned long long>(frames),
                    static_cast<unsigned long long>(g_stats.framesDropped.load()),
                    static_cast<unsigned long long>(g_stats.fragmentsLost.load()),
                    static_cast<unsigned long long>(g_stats.outOfOrder.load()),
                    static_cast<unsigned long long>(g_stats.keyframes.load()));

        previousBytes = bytes;
        previousFrames = frames;
    }
}

void PrintLocalAddresses()
{
    char hostname[256]{};
    ::gethostname(hostname, sizeof(hostname));

    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* results = nullptr;
    if (::getaddrinfo(hostname, nullptr, &hints, &results) != 0)
    {
        return;
    }

    std::printf("\nPoint the Xbox at one of these addresses:\n");
    for (addrinfo* it = results; it != nullptr; it = it->ai_next)
    {
        char text[INET_ADDRSTRLEN]{};
        auto* addr = reinterpret_cast<sockaddr_in*>(it->ai_addr);
        ::inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text));
        std::printf("   %s      (viewer: http://%s:%u)\n", text, text, kWebPort);
    }
    ::freeaddrinfo(results);
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv)
{
    // Without this the tool quits the moment stdin reaches EOF, which happens whenever it
    // is launched from a script or with redirected input rather than a real console.
    bool waitForEnter = true;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--no-wait")
        {
            waitForEnter = false;
        }
    }

    WSADATA wsaData{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::printf("WSAStartup failed\n");
        return 1;
    }

    std::printf("XVR stream receiver - protocol v%u\n", xvr::net::kProtocolVersion);
    PrintLocalAddresses();

    std::thread(ReceiveThread).detach();
    std::thread(WebServerThread).detach();
    std::thread(StatsThread).detach();

    std::printf("Viewer on http://localhost:%u  (also reachable from a phone on the same "
                "network)\nWriting received-eye0.h264 / received-eye1.h264\n%s\n\n",
                kWebPort, waitForEnter ? "Press Enter to quit." : "Running until killed.");

    if (waitForEnter)
    {
        std::getchar();
    }
    else
    {
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    ::WSACleanup();
    return 0;
}
