#pragma once

#include "xvr/Constraints.h"
#include "xvr/EncodedFrameSink.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <d3d11_4.h>
#include <mfidl.h>
#include <mftransform.h>

#include <winrt/base.h>

namespace xvr {

// One hardware H.264 encoder instance.
//
// Hardware encoder MFTs are typically *asynchronous*: they do not encode on the calling
// thread, they raise METransformNeedInput / METransformHaveOutput events and expect to
// be fed accordingly. This class handles both that and the simpler synchronous case.
class VideoEncoder
{
public:
    struct Stats
    {
        uint64_t framesSubmitted = 0;
        uint64_t framesEncoded = 0;
        uint64_t bytesEncoded = 0;
        // Frames the encoder never asked for, and which were therefore never submitted.
        uint64_t framesDropped = 0;
    };

    // Time from handing a frame to the encoder until its compressed output comes back.
    // This is pure render-to-photon cost, so it is measured rather than assumed.
    struct LatencyStats
    {
        double minMs = 0.0;
        double avgMs = 0.0;
        double maxMs = 0.0;
        uint64_t samples = 0;
    };

    // Throws if the configuration exceeds the measured hardware limits, rather than
    // letting the driver fail obscurely - or crash, in one case.
    void Initialize(ID3D11Device* device, const FrameConfig& config, IEncodedFrameSink* sink);

    /**
     * Asks the encoder to make the next frame an IDR.
     *
     * Called when the client reports it could not assemble a frame. Without this, a single
     * lost UDP fragment corrupts every following frame until the next scheduled keyframe -
     * up to a full GOP, which at the configured GOP size is about a second of visible
     * smearing from one dropped packet. Thread-safe: the request is a flag, applied on the
     * encoder thread before the next submit.
     */
    void RequestKeyframe();

    /**
     * Changes the target bitrate on a running encoder.
     *
     * The link decides what it can carry, not the encoder. 1920x1088 was chosen because it
     * is the largest frame the hardware accepts, and 25 Mbps went with it - per eye, so
     * fifty on the wire - without anyone asking what the Wi-Fi between the console and the
     * headset could actually sustain. When it cannot, the excess is dropped, and a dropped
     * fragment costs a whole frame.
     */
    void SetBitrate(uint32_t bitrateBps);

    uint32_t CurrentBitrate() const { return m_currentBitrateBps; }

    // `frameIndex` and `captureTimeUs` are carried through to the encoded output so the
    // sink can put them on the wire; the encoder itself does not interpret them.
    void Submit(ID3D11Texture2D* nv12Texture, int64_t timestampHns, uint32_t frameIndex = 0,
                uint64_t captureTimeUs = 0, uint32_t poseSequence = 0);

    // Which eye this encoder serves. Stamped onto every frame it produces.
    void SetEye(uint8_t eye) { m_eye = eye; }

    // Flushes frames still inside the encoder. Call before reading final stats.
    void Drain();
    void Shutdown();

    const Stats& GetStats() const { return m_stats; }
    LatencyStats GetLatencyStats() const;

    // Discards latency samples collected so far. Warm-up frames are submitted flat out
    // and leave a backlog inside the encoder; measuring across that boundary reports
    // queue depth from the warm-up rather than the encoder's actual per-frame cost.
    void ResetLatencyStats();
    bool IsAsynchronous() const { return m_async; }
    const std::wstring& EncoderName() const { return m_name; }

private:
    void ProbeCodecCapabilities();
    void ApplyPendingBitrate();
    void ApplyPendingKeyframeRequest();
    void ConfigureCodec();
    void EventThread();
    void AwaitNeedInput();
    bool ProcessOneOutput();

    winrt::com_ptr<IMFActivate> m_activate;
    winrt::com_ptr<IMFTransform> m_transform;
    winrt::com_ptr<IMFMediaEventGenerator> m_events;
    winrt::com_ptr<IMFDXGIDeviceManager> m_deviceManager;

    IEncodedFrameSink* m_sink = nullptr;
    FrameConfig m_config;
    Stats m_stats;
    std::wstring m_name;

    bool m_async = false;
    bool m_providesSamples = false;
    bool m_platformHeld = false;
    int64_t m_frameDurationHns = 0;

    // A dedicated thread blocks on the transform's event queue and dispatches each encoded
    // frame the moment it is ready.
    //
    // Draining output only when the next frame is submitted gives away a full frame
    // interval on every frame - the encoder finishes in a few milliseconds and then the
    // result sits there until the caller happens to come back. On a live VR stream that is
    // latency spent for nothing.
    std::thread m_eventThread;
    std::atomic<bool> m_eventThreadRunning{ false };
    std::atomic<bool> m_keyframeRequested{ false };
    uint64_t m_keyframesForced = 0;
    uint32_t m_currentBitrateBps = 0;
    std::atomic<uint32_t> m_pendingBitrateBps{ 0 };

    std::mutex m_creditMutex;
    std::condition_variable m_creditSignal;
    int m_needInputCredits = 0;
    bool m_drainComplete = false;

    // Guards the submit-time bookkeeping, which the event thread reads when output
    // arrives and the caller writes when a frame goes in.
    std::mutex m_submitMutex;

    struct SubmitRecord
    {
        std::chrono::steady_clock::time_point submittedAt;
        uint32_t frameIndex = 0;
        uint64_t captureTimeUs = 0;
        uint32_t poseSequence = 0;
    };

    // Keyed by sample timestamp, so encoder output can be matched back to its input.
    std::map<int64_t, SubmitRecord> m_submitTimes;
    uint8_t m_eye = 0;
    double m_latencyTotalMs = 0.0;
    double m_latencyMinMs = 0.0;
    double m_latencyMaxMs = 0.0;
    uint64_t m_latencySamples = 0;
};

} // namespace xvr
