#pragma once

// Sends encoded frames to the client as UDP fragments.
//
// Drops straight into the pipeline in place of FileFrameSink - the encoder and everything
// above it are unchanged, which is the point of IEncodedFrameSink existing.

#include "xvr/EncodedFrameSink.h"

#include <cstdint>
#include <string>

namespace xvr {

class UdpFrameSink : public IEncodedFrameSink
{
public:
    struct Stats
    {
        uint64_t framesSent = 0;
        uint64_t fragmentsSent = 0;
        uint64_t bytesSent = 0;
        uint64_t sendFailures = 0;
        uint64_t oversizeFrames = 0;
    };

    ~UdpFrameSink() override;

    void Initialize(const std::string& destinationHost, uint16_t destinationPort, uint8_t eye);

    // Creates the socket without a destination yet. Frames are discarded until
    // SetDestination is called, which is what lets the host start up before any client
    // has announced itself.
    void InitializeUnbound(uint8_t eye);

    /**
     * Tells the sink how long a frame lasts, so it can spread a frame's fragments over that
     * time instead of emitting them as fast as sendto returns.
     *
     * Without this a moving frame - which is far larger than a still one - goes out as one
     * burst of a hundred-odd datagrams in microseconds, and both eyes burst together. The
     * local socket buffer absorbs it happily; the air and the receiver do not, and the
     * excess is dropped. That is why corruption appears when moving and not when still.
     */
    void SetFrameInterval(uint32_t frameRate)
    {
        m_frameIntervalUs = frameRate > 0 ? 1'000'000 / frameRate : 0;
    }
    void SetDestination(const unsigned char* address, int addressSize);

    /** Stops sending until a new destination is supplied. */
    void ClearDestination() { m_destinationSize = 0; }

    bool HasDestination() const { return m_destinationSize != 0; }

    void Shutdown();

    void OnEncodedFrame(const EncodedFrame& frame) override;

    const Stats& GetStats() const { return m_stats; }

private:
    bool SendDatagram(const void* data, size_t size);

    uintptr_t m_socket = ~static_cast<uintptr_t>(0); // INVALID_SOCKET without windows.h here
    bool m_winsockStarted = false;
    uint8_t m_eye = 0;
    uint32_t m_frameIntervalUs = 0;
    Stats m_stats;

    // Reused per fragment so a 72fps stream is not allocating thousands of times a second.
    std::string m_packetBuffer;
    // Filled in by Initialize; kept opaque so this header stays free of winsock.
    unsigned char m_destination[32]{};
    int m_destinationSize = 0;
};

} // namespace xvr
