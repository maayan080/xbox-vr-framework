#pragma once

// Finds the client and establishes where video should be sent.
//
// Neither side is configured with the other's address. The client broadcasts a Handshake
// on the local subnet; the host replies to whatever address it came from and streams
// there. That matters because the host is a console: entering an IP address with a
// controller is miserable, and a wrong digit is indistinguishable from a network fault.

#include "xvr/Constraints.h"
#include "xvr/StereoRenderer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace xvr {

class ClientLink
{
public:
    struct Endpoint
    {
        unsigned char address[32]{};
        int size = 0;
        std::string text;   // human-readable, for logs
        uint16_t port = 0;

        // What the client asked for. The client knows its own display refresh rate; the
        // host has no way to guess it, and guessing wrong means either judder or wasted
        // encode capacity.
        uint16_t requestedWidth = 0;
        uint16_t requestedHeight = 0;
        uint8_t requestedFrameRate = 0;
    };

    ~ClientLink();

    // Binds the discovery port. Non-blocking; nothing waits on a client.
    bool Start(uint16_t discoveryPort = 9943);
    void Stop();

    // Checks for a client announcement and replies with the actual configuration. Returns
    // true when a client is found, filling `endpoint` with where to send video.
    bool PollForClient(const FrameConfig& config, Endpoint& endpoint);

    /**
     * Drains pending pose packets and applies the newest to `frame`.
     *
     * Only the highest sequence number survives: an older pose arriving late carries no
     * information, because a newer one has already superseded it. Draining the whole
     * queue each call also stops a backlog building up, which would feed the renderer
     * progressively staler poses - latency that grows the longer the session runs.
     */
    bool PollPose(FrameContext& frame);

    bool HasPose() const { return m_havePose; }

    /**
     * True once if the client asked for a keyframe since the last call.
     *
     * The client sends this when it fails to assemble a frame. Consuming the flag rather
     * than exposing a count is deliberate: several requests arriving during one bad patch
     * of Wi-Fi should still produce a single keyframe, not a burst of them - a keyframe is
     * far larger than a P-frame, so answering every request would put the most data on the
     * link at exactly the moment it is already dropping packets.
     */
    bool ConsumeKeyframeRequest()
    {
        return m_keyframeRequested.exchange(false, std::memory_order_acq_rel);
    }
    uint64_t PosePacketsReceived() const { return m_posePackets; }
    // Counted separately so "no poses arriving" and "poses arriving but discarded" are
    // distinguishable. Confusing the two cost a debugging round trip.
    uint64_t PosesRejected() const { return m_posesRejected; }

    bool IsListening() const { return m_socket != ~static_cast<uintptr_t>(0); }

private:
    // Reads every pending packet and files it by type. Both poll functions call this,
    // because they share one socket - if each drained independently, one would silently
    // consume and discard the other's packets.
    void DrainSocket(const FrameConfig& config);

    uintptr_t m_socket = ~static_cast<uintptr_t>(0);
    bool m_winsockStarted = false;

    Endpoint m_pendingEndpoint;
    bool m_hasPendingEndpoint = false;

    bool m_warnedVersionMismatch = false;
    bool m_havePose = false;
    std::atomic<bool> m_keyframeRequested{ false };
    uint32_t m_latestSequence = 0;
    uint64_t m_posePackets = 0;
    uint64_t m_posesRejected = 0;
    FrameContext m_latestPose;
    std::chrono::steady_clock::time_point m_poseReceivedAt;
};

} // namespace xvr
