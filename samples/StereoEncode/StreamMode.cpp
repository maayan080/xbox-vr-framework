#include "StreamMode.h"

#include "SceneRenderer.h"

#include "xvr/Check.h"
#include "xvr/ClientLink.h"
#include "xvr/Constraints.h"
#include "xvr/Log.h"
#include "xvr/StereoEncodePipeline.h"
#include "xvr/UdpFrameSink.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <iterator>

namespace sample {
namespace {

// Used only if a client does not state a rate. Frames are delivered at the client's
// display refresh, which is higher than the rate the media type declares - the declared
// rate only has to satisfy the encoder's MaxMBPS limit, and the encoder accepts input
// faster than that without complaint.
constexpr uint32_t kDefaultDeliveryFrameRate = 72;

// Bounds on what a client may ask for. Two encoders sustained 141.6 fps each at
// 1920x1088 on the console, so 120 is achievable with headroom; anything beyond would be
// accepted and then silently missed, which is worse than refusing it.
constexpr uint32_t kMaxDeliveryFrameRate = 120;
constexpr uint32_t kMinDeliveryFrameRate = 30;

} // namespace

struct StreamSession::Impl
{
    xvr::FrameConfig config = xvr::kMaxResolutionPerEyeConfig;
    SceneRenderer renderer;
    xvr::UdpFrameSink sinks[2];
    xvr::StereoEncodePipeline pipeline;
    xvr::ClientLink link;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    bool clientConnected = false;
    bool pipelineReady = false;
    std::string clientAddress;
    uint64_t frameIndex = 0;
    uint32_t deliveryFrameRate = kDefaultDeliveryFrameRate;
    uint64_t resyncs = 0;
    uint64_t framesSkipped = 0;
    uint64_t lateFrames = 0;
    double lastPoseAgeMs = 0.0;
    bool havePose = false;

    std::chrono::steady_clock::time_point nextFrameDue;
    // Last time a valid pose arrived, used to notice a client that has gone away.
    std::chrono::steady_clock::time_point lastPoseTime;

    // Rate adaptation. A rate the hardware can actually hold looks dramatically better
    // than a higher one it keeps missing, so the target steps down rather than stuttering.
    std::chrono::steady_clock::time_point rateWindowStart;
    uint64_t rateWindowFrames = 0;
    uint32_t requestedFrameRate = kDefaultDeliveryFrameRate;
    double measuredFps = 0.0;
    uint32_t rateReductions = 0;
    uint64_t lateFramesInWindow = 0;
    uint32_t goodWindows = 0;

    std::chrono::steady_clock::time_point lastPoll;
    std::chrono::steady_clock::time_point streamStart;
};

bool StreamSession::Start(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_impl = new Impl();
    m_impl->device = device;
    m_impl->context = context;

    std::wstring reason;
    if (!m_impl->config.Validate(reason))
    {
        xvr::LogError(L"Stream configuration rejected: {}", reason);
        return false;
    }

    m_impl->sinks[0].InitializeUnbound(0);
    m_impl->sinks[1].InitializeUnbound(1);

    if (!m_impl->link.Start())
    {
        xvr::LogError(L"Could not start client discovery");
        return false;
    }

    m_impl->lastPoll = std::chrono::steady_clock::now();

    // The encoders are deliberately not created yet. The delivery frame rate is not known
    // until a client asks for one, and it determines the bitrate compensation - so
    // building the pipeline now would mean tearing it down and rebuilding it moments
    // later. It also avoids holding the console's encode block while nothing is watching.
    xvr::LogInfo(L"Stream session ready: {}x{} per eye, waiting for a client",
                 m_impl->config.width, m_impl->config.height);
    return true;
}

void StreamSession::Tick(std::vector<std::wstring>& headline, std::vector<std::wstring>& detail)
{
    if (!m_impl)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (!m_impl->clientConnected)
    {
        // Polled a few times a second rather than every frame; discovery is not urgent and
        // a syscall per frame would be pure waste once streaming starts.
        if (now - m_impl->lastPoll > std::chrono::milliseconds(200))
        {
            m_impl->lastPoll = now;

            xvr::ClientLink::Endpoint endpoint;
            if (m_impl->link.PollForClient(m_impl->config, endpoint))
            {
                // Honour what the client asked for, within what the hardware can hold.
                // The client knows its own refresh rate; most Quests default to 72 but the
                // user may have set 90 or 120, and only the client can tell us which.
                uint32_t requested = endpoint.requestedFrameRate != 0
                                         ? endpoint.requestedFrameRate
                                         : kDefaultDeliveryFrameRate;
                const uint32_t clamped =
                    std::clamp(requested, kMinDeliveryFrameRate, kMaxDeliveryFrameRate);

                if (clamped != requested)
                {
                    xvr::LogWarn(L"Client asked for {}fps; clamped to {}", requested, clamped);
                }
                m_impl->deliveryFrameRate = clamped;

                // CBR spends bits per *declared* second. Delivering more frames than the
                // declaration would otherwise overshoot the bitrate by exactly that ratio,
                // so the declared figure is scaled to compensate.
                xvr::FrameConfig encoderConfig = m_impl->config;
                encoderConfig.bitrateBps = static_cast<uint32_t>(
                    static_cast<uint64_t>(encoderConfig.bitrateBps) * encoderConfig.frameRate /
                    m_impl->deliveryFrameRate);

                try
                {
                    m_impl->pipeline.Initialize(m_impl->device, m_impl->context, encoderConfig,
                                                &m_impl->renderer,
                                                { &m_impl->sinks[0], &m_impl->sinks[1] }, 2);
                    m_impl->pipelineReady = true;
                }
                catch (const xvr::HresultException& e)
                {
                    xvr::LogError(L"Pipeline init failed: 0x{:08X}",
                                  static_cast<uint32_t>(e.Code()));
                    return;
                }

                m_impl->sinks[0].SetDestination(endpoint.address, endpoint.size);
                m_impl->sinks[1].SetDestination(endpoint.address, endpoint.size);
                m_impl->clientConnected = true;
                m_impl->clientAddress = endpoint.text;
                m_impl->streamStart = now;
                m_impl->nextFrameDue = now;
                m_impl->rateWindowStart = now;
                // Started here so a client that connects but never sends a pose is still
                // timed out rather than streamed to indefinitely.
                m_impl->lastPoseTime = now;
                m_impl->requestedFrameRate = m_impl->deliveryFrameRate;

                xvr::LogInfo(L"Streaming to {} at {}fps",
                             std::wstring(endpoint.text.begin(), endpoint.text.end()),
                             m_impl->deliveryFrameRate);
            }
        }

        headline = { L"WAITING FOR CLIENT",
                     std::format(L"{}x{} per eye", m_impl->config.width, m_impl->config.height) };
        detail = { L"Run the client on a device on the same network.",
                   L"It announces itself and states its refresh rate;",
                   L"no address needs to be entered here.",
                   L"",
                   L"PC: XvrStreamReceiver.exe    Quest/phone: XvrClient.apk" };
        return;
    }

    if (!m_impl->pipelineReady)
    {
        return;
    }

    // Give up on a client that has stopped talking.
    //
    // Poses arrive every client frame, so silence means the headset is gone - closed,
    // crashed, or off the network. Continuing to encode and transmit into that wastes the
    // encode block and power, and leaves the host unable to accept a new client because it
    // still believes it has one. Returning to discovery is what makes restarting either
    // side recover on its own.
    // Timed here rather than read from the pose age, which stops advancing: PollPose
    // reports no pose at all once one is over a second old, so its age would sit frozen at
    // whatever it last was and never cross a timeout.
    const double silentMs =
        std::chrono::duration<double, std::milli>(now - m_impl->lastPoseTime).count();

    if (silentMs > 3000.0)
    {
        xvr::LogWarn(L"Client silent for {:.0f}ms; returning to discovery", silentMs);

        m_impl->pipeline.Drain();
        m_impl->pipeline.Shutdown();
        m_impl->pipelineReady = false;
        m_impl->clientConnected = false;
        m_impl->havePose = false;
        m_impl->frameIndex = 0;
        m_impl->sinks[0].ClearDestination();
        m_impl->sinks[1].ClearDestination();
        return;
    }

    // Paced to the negotiated rate rather than free-running, but the deadline advances
    // from the previous frame rather than from a fixed origin.
    //
    // Deriving every deadline from a single start time accumulates debt: a host that can
    // only manage 100fps against a 120fps target falls a fraction behind on every frame,
    // and when that debt crosses a threshold it gets discharged all at once - several
    // frames dropped together. The result is smooth motion punctuated by a regular lurch,
    // which is far worse to look at than simply running slower. Slipping the deadline
    // frame by frame degrades smoothly to whatever the hardware can actually sustain.
    const auto frameInterval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / m_impl->deliveryFrameRate));

    if (now < m_impl->nextFrameDue)
    {
        return;
    }

    m_impl->nextFrameDue += frameInterval;
    if (m_impl->nextFrameDue < now)
    {
        // Already late for the following frame too: give up the backlog rather than
        // carrying it forward, but only ever one frame at a time.
        m_impl->nextFrameDue = now + frameInterval;
        m_impl->lateFrames++;
        m_impl->lateFramesInWindow++;
    }

    // Poll immediately before rendering so the freshest pose is used. Anything older is
    // motion-to-photon latency the user feels directly.
    xvr::FrameContext frame;
    frame.frameIndex = m_impl->frameIndex;
    frame.timeSeconds = static_cast<double>(m_impl->frameIndex) / m_impl->deliveryFrameRate;
    m_impl->link.PollPose(frame);

    m_impl->pipeline.RenderAndEncodeFrame(frame);
    ++m_impl->frameIndex;
    ++m_impl->rateWindowFrames;

    m_impl->lastPoseAgeMs = frame.poseAgeMs;
    m_impl->havePose = frame.poseValid;
    if (frame.poseValid)
    {
        m_impl->lastPoseTime = now;
    }

    // Adapt the target to what the hardware actually sustains, in both directions.
    //
    // The signal is the proportion of frames delivered late, not the measured frame rate:
    // when pacing works, measured always equals target, so comparing the two only ever
    // reacts to transients. Late frames are the honest measure of being unable to keep up.
    //
    // Recovery upward matters as much as stepping down. Without it every transient - a
    // keyframe spike, a brief network stall - costs a rung permanently, and a healthy
    // pipeline ratchets itself down to a crawl. Hysteresis rather than one-way descent:
    // quick to back off, slow and deliberate to climb.
    const double windowSeconds =
        std::chrono::duration<double>(now - m_impl->rateWindowStart).count();
    if (windowSeconds >= 2.0 && m_impl->rateWindowFrames > 0)
    {
        static constexpr uint32_t kRateLadder[] = { 30, 45, 60, 72, 90, 120 };

        m_impl->measuredFps = static_cast<double>(m_impl->rateWindowFrames) / windowSeconds;
        const double lateRatio = static_cast<double>(m_impl->lateFramesInWindow) /
                                 static_cast<double>(m_impl->rateWindowFrames);

        if (lateRatio > 0.15 && m_impl->deliveryFrameRate > kMinDeliveryFrameRate)
        {
            // Struggling: drop one rung immediately.
            uint32_t nextRate = m_impl->deliveryFrameRate;
            for (int i = static_cast<int>(std::size(kRateLadder)) - 1; i >= 0; --i)
            {
                if (kRateLadder[i] < m_impl->deliveryFrameRate)
                {
                    nextRate = kRateLadder[i];
                    break;
                }
            }

            if (nextRate != m_impl->deliveryFrameRate)
            {
                xvr::LogWarn(L"{:.0f}% of frames late at {}fps; stepping down to {}",
                             lateRatio * 100.0, m_impl->deliveryFrameRate, nextRate);
                m_impl->deliveryFrameRate = nextRate;
                m_impl->rateReductions++;
            }
            m_impl->goodWindows = 0;
        }
        else if (lateRatio < 0.02)
        {
            ++m_impl->goodWindows;

            // Three clean windows - six seconds - before climbing. Slow enough that it
            // does not oscillate around a rate the hardware can only just manage.
            if (m_impl->goodWindows >= 3 &&
                m_impl->deliveryFrameRate < m_impl->requestedFrameRate)
            {
                uint32_t nextRate = m_impl->deliveryFrameRate;
                for (const uint32_t rate : kRateLadder)
                {
                    if (rate > m_impl->deliveryFrameRate && rate <= m_impl->requestedFrameRate)
                    {
                        nextRate = rate;
                        break;
                    }
                }

                if (nextRate != m_impl->deliveryFrameRate)
                {
                    xvr::LogInfo(L"Stable at {}fps; stepping up to {}",
                                 m_impl->deliveryFrameRate, nextRate);
                    m_impl->deliveryFrameRate = nextRate;
                }
                m_impl->goodWindows = 0;
            }
        }
        else
        {
            m_impl->goodWindows = 0;
        }

        m_impl->rateWindowStart = now;
        m_impl->rateWindowFrames = 0;
        m_impl->lateFramesInWindow = 0;
    }

    const double elapsed = std::chrono::duration<double>(now - m_impl->streamStart).count();
    const double fps = elapsed > 0.0 ? static_cast<double>(m_impl->frameIndex) / elapsed : 0.0;

    const auto& left = m_impl->sinks[0].GetStats();
    const auto& right = m_impl->sinks[1].GetStats();
    const double mbps =
        elapsed > 0.0 ? (left.bytesSent + right.bytesSent) * 8.0 / 1'000'000.0 / elapsed : 0.0;

    headline = { L"STREAMING",
                 std::format(L"to {}", std::wstring(m_impl->clientAddress.begin(),
                                                    m_impl->clientAddress.end())),
                 std::format(L"{:.0f} fps   {:.1f} Mbps", fps, mbps) };

    // Submitted vs encoded is the encoder's internal queue depth, and queued frames are
    // latency. A gap that grows means the encoder is falling behind the delivery rate.
    const auto& leftEncoder = m_impl->pipeline.Encoder(0).GetStats();
    const auto& rightEncoder = m_impl->pipeline.Encoder(1).GetStats();

    detail = {
        std::format(L"{}x{} per eye, delivering {}fps ({}fps declared)", m_impl->config.width,
                    m_impl->config.height, m_impl->deliveryFrameRate, m_impl->config.frameRate),
        std::format(L"submitted     L {}   R {}", leftEncoder.framesSubmitted,
                    rightEncoder.framesSubmitted),
        std::format(L"encoded       L {}   R {}   (in flight L {}  R {})",
                    leftEncoder.framesEncoded, rightEncoder.framesEncoded,
                    leftEncoder.framesSubmitted - leftEncoder.framesEncoded,
                    rightEncoder.framesSubmitted - rightEncoder.framesEncoded),
        std::format(L"sent          L {}   R {}", left.framesSent, right.framesSent),
        std::format(L"fragments     L {}   R {}", left.fragmentsSent, right.fragmentsSent),
        std::format(L"send failures L {}   R {}", left.sendFailures, right.sendFailures),
        std::format(L"target {}fps   measured {:.1f}fps   late {}   stepdowns {}",
                    m_impl->deliveryFrameRate, m_impl->measuredFps, m_impl->lateFrames,
                    m_impl->rateReductions),
        m_impl->havePose
            ? std::format(L"pose: {} accepted, {} rejected, {:.1f}ms old",
                          m_impl->link.PosePacketsReceived(), m_impl->link.PosesRejected(),
                          m_impl->lastPoseAgeMs)
            : std::wstring(L"pose: NONE - rendering a fixed viewpoint"),
    };
}

void StreamSession::Stop()
{
    if (!m_impl)
    {
        return;
    }

    if (m_impl->pipelineReady)
    {
        m_impl->pipeline.Drain();
        m_impl->pipeline.Shutdown();
    }

    m_impl->sinks[0].Shutdown();
    m_impl->sinks[1].Shutdown();
    m_impl->link.Stop();

    delete m_impl;
    m_impl = nullptr;
}

} // namespace sample
