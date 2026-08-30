#include "xvr/openxr/XrRuntime.h"

#include "xvr/Check.h"
#include "xvr/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

#include <winrt/Windows.System.h>

namespace xvr::xr {
namespace {

constexpr int64_t kHnsPerSecond = 10'000'000;
constexpr int64_t kNsPerSecond = 1'000'000'000;

std::chrono::steady_clock::time_point g_epoch = std::chrono::steady_clock::now();

} // namespace

// --- Paths ------------------------------------------------------------------------------

XrPath PathTable::Intern(const std::string& text)
{
    std::lock_guard lock(m_mutex);
    for (size_t i = 0; i < m_paths.size(); ++i)
    {
        if (m_paths[i] == text)
        {
            return static_cast<XrPath>(i + 1); // 0 is XR_NULL_PATH
        }
    }
    m_paths.push_back(text);
    return static_cast<XrPath>(m_paths.size());
}

bool PathTable::Lookup(XrPath path, std::string& text) const
{
    std::lock_guard lock(m_mutex);
    if (path == XR_NULL_PATH || path > m_paths.size())
    {
        return false;
    }
    text = m_paths[static_cast<size_t>(path) - 1];
    return true;
}

// --- Events -----------------------------------------------------------------------------

void Instance::QueueSessionState(XrSession sessionHandle, XrSessionState state)
{
    XrEventDataSessionStateChanged changed{ XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED };
    changed.session = sessionHandle;
    changed.state = state;
    changed.time = NowXrTime();

    XrEventDataBuffer buffer{};
    std::memcpy(&buffer, &changed, sizeof(changed));

    std::lock_guard lock(eventMutex);
    events.push_back(buffer);
}

// --- Pose maths -------------------------------------------------------------------------

XrPosef IdentityPose()
{
    return XrPosef{ { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
}

XrPosef ToXrPose(const float position[3], const float orientation[4])
{
    XrPosef pose;
    pose.orientation.x = orientation[0];
    pose.orientation.y = orientation[1];
    pose.orientation.z = orientation[2];
    pose.orientation.w = orientation[3];
    pose.position.x = position[0];
    pose.position.y = position[1];
    pose.position.z = position[2];

    // The client sends what its runtime reported, but a zeroed struct arrives as a
    // degenerate quaternion. Rendering with one produces a collapsed view rather than an
    // error, so it is repaired here where it can still be noticed.
    const float lengthSq = pose.orientation.x * pose.orientation.x +
                           pose.orientation.y * pose.orientation.y +
                           pose.orientation.z * pose.orientation.z +
                           pose.orientation.w * pose.orientation.w;
    if (lengthSq < 1e-8f)
    {
        pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
    }
    return pose;
}

namespace {

XrQuaternionf QuatMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
    return XrQuaternionf{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

XrQuaternionf QuatConjugate(const XrQuaternionf& q)
{
    return XrQuaternionf{ -q.x, -q.y, -q.z, q.w };
}

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v)
{
    // v + 2w(q x v) + 2(q x (q x v)), which avoids building a matrix for one vector.
    const XrVector3f u{ q.x, q.y, q.z };
    const XrVector3f uv{ u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
    const XrVector3f uuv{ u.y * uv.z - u.z * uv.y, u.z * uv.x - u.x * uv.z,
                          u.x * uv.y - u.y * uv.x };
    return XrVector3f{
        v.x + 2.0f * (q.w * uv.x + uuv.x),
        v.y + 2.0f * (q.w * uv.y + uuv.y),
        v.z + 2.0f * (q.w * uv.z + uuv.z),
    };
}

} // namespace

XrPosef Multiply(const XrPosef& parent, const XrPosef& child)
{
    XrPosef out;
    out.orientation = QuatMul(parent.orientation, child.orientation);
    const XrVector3f rotated = Rotate(parent.orientation, child.position);
    out.position = { parent.position.x + rotated.x, parent.position.y + rotated.y,
                     parent.position.z + rotated.z };
    return out;
}

XrPosef Invert(const XrPosef& pose)
{
    XrPosef out;
    out.orientation = QuatConjugate(pose.orientation);
    const XrVector3f negated{ -pose.position.x, -pose.position.y, -pose.position.z };
    out.position = Rotate(out.orientation, negated);
    return out;
}

XrTime NowXrTime()
{
    const auto elapsed = std::chrono::steady_clock::now() - g_epoch;
    return static_cast<XrTime>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

// --- Binding resolution -----------------------------------------------------------------

int HandFromPath(const std::string& path)
{
    if (path.rfind("/user/hand/left", 0) == 0)
    {
        return 0;
    }
    if (path.rfind("/user/hand/right", 0) == 0)
    {
        return 1;
    }
    return -1;
}

Action::Binding BindingFromPath(const std::string& path)
{
    const size_t input = path.find("/input/");
    if (input == std::string::npos)
    {
        return Action::Binding::None;
    }
    const std::string component = path.substr(input + 7);

    // Matched on prefix so that "/trigger/value", "/trigger/click" and a bare "/trigger"
    // all land on the same control. Which suffix an application asks for varies, and the
    // underlying float is the same either way.
    if (component.rfind("grip/pose", 0) == 0 || component.rfind("grip_surface/pose", 0) == 0)
    {
        return Action::Binding::GripPose;
    }
    if (component.rfind("aim/pose", 0) == 0 || component.rfind("pointer/pose", 0) == 0)
    {
        return Action::Binding::AimPose;
    }
    if (component.rfind("trigger", 0) == 0)
    {
        return Action::Binding::Trigger;
    }
    if (component.rfind("squeeze", 0) == 0)
    {
        return Action::Binding::Squeeze;
    }
    if (component.rfind("thumbstick/click", 0) == 0)
    {
        return Action::Binding::ThumbstickClick;
    }
    if (component.rfind("thumbstick", 0) == 0)
    {
        return Action::Binding::Thumbstick;
    }
    if (component.rfind("menu", 0) == 0 || component.rfind("system", 0) == 0)
    {
        return Action::Binding::MenuClick;
    }
    // A/X sit in the primary position on their respective hands, B/Y in the secondary one.
    if (component.rfind("a/", 0) == 0 || component == "a" || component.rfind("x/", 0) == 0 ||
        component == "x")
    {
        return Action::Binding::PrimaryClick;
    }
    if (component.rfind("b/", 0) == 0 || component == "b" || component.rfind("y/", 0) == 0 ||
        component == "y")
    {
        return Action::Binding::SecondaryClick;
    }
    return Action::Binding::None;
}

// --- Session ----------------------------------------------------------------------------

namespace {

/**
 * Runs `body`, turning anything it throws into an XrResult.
 *
 * The entry points are extern "C" and the application on the other side of them is under no
 * obligation to have a handler - a C++ exception crossing that boundary terminates the
 * process with nothing useful printed. The framework signals failure by throwing
 * (XVR_CHECK), so the two conventions have to meet somewhere, and this is it.
 */
template <typename Body>
XrResult Guard(const wchar_t* what, Body&& body)
{
    try
    {
        return body();
    }
    catch (const HresultException& e)
    {
        LogError(L"OpenXR: {} failed with 0x{:08X}", what, static_cast<uint32_t>(e.Code()));
    }
    catch (const std::exception& e)
    {
        LogError(L"OpenXR: {} failed: {}", what, std::wstring(e.what(), e.what() + std::strlen(e.what())));
    }
    catch (...)
    {
        LogError(L"OpenXR: {} failed with an unknown exception", what);
    }
    return XR_ERROR_RUNTIME_FAILURE;
}

bool MarkerFilePresent(const wchar_t* name)
{
    // Deliberately crude: a console has no command line and this build has no settings UI,
    // so dropping an empty file next to the logs is the only switch a user actually has.
    std::wstring path = LogDirectory();
    if (path.empty())
    {
        return false;
    }
    path += L'\\';
    path += name;

    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES;
}

XrResult StartPipelineImpl(Session& session)
{
    if (session.pipelineReady)
    {
        return XR_SUCCESS;
    }

    // Escape hatch. Encoding and streaming are the parts most likely to take a console down
    // with them, and when that happens there is no way to tell whether the emulator alone is
    // healthy. Dropping xvr-no-encode.txt beside the log runs the whole OpenXR session -
    // poses, swapchains, frame loop - while skipping NV12 conversion, the encoder and the
    // socket entirely, which isolates our half from the application's.
    //
    // XVR_FORCE_NO_ENCODE does the same at compile time, for hosts where dropping a file
    // into app storage is not actually possible - which includes a console whose only file
    // access is a Device Portal that will not write there.
#ifdef XVR_FORCE_NO_ENCODE
    session.encodeDisabled = true;
#else
    session.encodeDisabled = MarkerFilePresent(L"xvr-no-encode.txt");
#endif
    if (session.encodeDisabled)
    {
        LogWarn(L"Encoding and streaming are DISABLED for this run. The session, poses and "
                L"frame loop still run; the headset will stay black by design.");
    }

    std::wstring reason;
    if (!session.config.Validate(reason))
    {
        LogError(L"OpenXR session config rejected by encoder limits: {}", reason);
        return XR_ERROR_RUNTIME_FAILURE;
    }

    // The staging textures are deliberately not created here: their format has to match
    // whichever swapchain format the application chose, and that is not known until it
    // submits its first frame.
    for (size_t eye = 0; eye < 2 && !session.encodeDisabled; ++eye)
    {
        session.converters[eye].Initialize(session.device.get(), session.context.get(),
                                           session.config.width, session.config.height);
        session.sinks[eye].InitializeUnbound(static_cast<uint8_t>(eye));
        session.sinks[eye].SetFrameInterval(session.config.frameRate);

        session.encoders[eye] = std::make_unique<VideoEncoder>();
        session.encoders[eye]->SetEye(static_cast<uint8_t>(eye));
        session.encoders[eye]->Initialize(session.device.get(), session.config,
                                          &session.sinks[eye]);
    }

    if (!session.link.Start())
    {
        LogError(L"OpenXR: discovery socket failed to bind");
        return XR_ERROR_RUNTIME_FAILURE;
    }

    session.sessionStart = std::chrono::steady_clock::now();
    session.nextFrameDue = session.sessionStart;

    // Watchdog thread. CheckDeviceHealth only runs when a thread reaches it, and the failure
    // being chased is precisely one that stops the render thread - so something outside that
    // thread has to notice. If the frame clock has not moved for a few seconds the machine is
    // already in trouble; saying so and exiting is far better than sitting there until the
    // system watchdog tears down the display.
    session.lastFrameTick.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count(),
        std::memory_order_relaxed);
    session.watchdogRunning.store(true, std::memory_order_release);
    session.watchdog = std::thread([&session] {
        // Two thresholds, because the two failures being guarded against are not the same.
        //
        // A lost device is unrecoverable and every extra second risks the system watchdog
        // tearing the display down instead, so that exits immediately. A merely stalled thread
        // in front of a healthy GPU is survivable, and exiting at five seconds destroyed the
        // evidence: the app was killed before anyone could see whether it recovered, or how
        // long the stall actually was. Now that only reports, and gives it thirty seconds
        // before concluding it is never coming back.
        constexpr int64_t kReportStallMs = 5000;
        constexpr int64_t kFatalStallMs = 30000;
        bool reported = false;
        while (session.watchdogRunning.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
            const int64_t last = session.lastFrameTick.load(std::memory_order_relaxed);
            if (now - last < kReportStallMs)
            {
                if (reported)
                {
                    LogInfo(L"WATCHDOG: frames resumed at frame {}", session.frameIndex);
                    reported = false;
                }
                continue;
            }

            const HRESULT deviceReason =
                session.device ? session.device->GetDeviceRemovedReason() : S_OK;

            if (deviceReason == S_OK && (now - last) < kFatalStallMs)
            {
                if (!reported)
                {
                    reported = true;
                    LogWarn(L"WATCHDOG: no frame for {} ms at frame {}, but the device is "
                            L"healthy - waiting to see whether it recovers.",
                            now - last, session.frameIndex);
                }
                continue;
            }

            LogError(L"WATCHDOG: no frame submitted for {} ms, at frame {}. The render thread "
                     L"is stuck.",
                     now - last, session.frameIndex);

            // Ask the device what it thinks first - if the GPU is gone this names the reason,
            // which is the one piece of evidence a hung console otherwise destroys.
            if (session.device)
            {
                const HRESULT reason = session.device->GetDeviceRemovedReason();
                LogError(L"WATCHDOG: GetDeviceRemovedReason = 0x{:08X}",
                         static_cast<uint32_t>(reason));
            }
            LogError(L"WATCHDOG: exiting so the driver can reclaim the device.");
            std::_Exit(4);
        }
    });

    session.pipelineReady = true;

    LogInfo(L"OpenXR session streaming at {}x{} per eye, waiting for a client",
            session.config.width, session.config.height);
    return XR_SUCCESS;
}

/**
 * Walks the bitrate toward whatever the link will actually carry.
 *
 * Loss is inferred from keyframe requests: the client only asks when it failed to assemble
 * a frame, so a request in the last window means fragments were dropped. Backing off is
 * fast and recovery is slow, deliberately - overshooting costs a visibly broken picture for
 * as long as it takes to notice, while undershooting only costs some sharpness.
 *
 * Both eyes are always set to the same value. They share one radio, and letting them drift
 * apart would leave one eye sharper than the other, which is more unpleasant to look at
 * than both being slightly soft.
 */
void AdaptBitrate(Session& session)
{
    constexpr auto kCheckInterval = std::chrono::seconds(2);
    const auto now = std::chrono::steady_clock::now();

    if (session.currentBitrateBps == 0)
    {
        session.currentBitrateBps = session.config.bitrateBps;
        session.lastBitrateCheck = now;
        session.keyframeRequestsAtLastCheck = session.keyframeRequests;
        return;
    }

    if (now - session.lastBitrateCheck < kCheckInterval)
    {
        return;
    }
    session.lastBitrateCheck = now;

    const uint64_t requests = session.keyframeRequests - session.keyframeRequestsAtLastCheck;
    session.keyframeRequestsAtLastCheck = session.keyframeRequests;

    uint32_t target = session.currentBitrateBps;
    if (requests > 0)
    {
        // Down hard. The link is already dropping data, so easing off gently just prolongs
        // the period where the picture is broken.
        target = static_cast<uint32_t>(target * 0.75);
    }
    else
    {
        // Up gently, and only after a clean window.
        target = static_cast<uint32_t>(target * 1.1);
    }

    target = (std::max)(kAdaptiveMinBitrateBps, (std::min)(kAdaptiveMaxBitrateBps, target));
    if (target == session.currentBitrateBps)
    {
        return;
    }

    LogInfo(L"Adaptive bitrate: {} -> {} kbps ({} keyframe requests in the last {} s)",
            session.currentBitrateBps / 1000, target / 1000, requests,
            std::chrono::duration_cast<std::chrono::seconds>(kCheckInterval).count());

    session.currentBitrateBps = target;
    for (auto& encoder : session.encoders)
    {
        if (encoder)
        {
            encoder->SetBitrate(target);
        }
    }
}

XrResult WaitFrameImpl(Session& session, XrFrameState& state)
{
    if (const XrResult result = SessionStartPipeline(session); result != XR_SUCCESS)
    {
        return result;
    }

    // Discovery and the pose channel share a socket, so both are serviced every frame even
    // once a client is attached - otherwise the reply to a reconnecting client is never sent.
    ClientLink::Endpoint endpoint;
    if (session.link.PollForClient(session.config, endpoint))
    {
        for (auto& sink : session.sinks)
        {
            sink.SetDestination(endpoint.address, endpoint.size);
        }
        session.clientConnected = true;
        session.clientAddress = endpoint.text;

        if (endpoint.requestedFrameRate != 0)
        {
            // Clamped to the rate the encoder was configured for, which the client's request
            // has no say in - the encoder is built before any client connects.
            //
            // Honouring 90 against an encoder set up for 60 does not give 90 fps of the
            // requested quality. It gives 90 fps at one and a half times the requested
            // bitrate, because rate control budgets bits per frame assuming 60 of them make
            // a second. Every sample is also stamped as lasting 1/60 s while arriving every
            // 1/90, so the timeline the decoder is handed is not the one frames arrive on.
            //
            // The bitrate reaching the air is the one thing this path exists to control, so
            // quietly exceeding it by half is worse than delivering fewer frames.
            session.deliveryFrameRate =
                (std::min)(static_cast<uint32_t>(endpoint.requestedFrameRate), session.config.frameRate);

            if (endpoint.requestedFrameRate > session.config.frameRate)
            {
                LogInfo(L"OpenXR: client asked for {} fps; the encoder is configured for {}, "
                        L"so delivering {}",
                        endpoint.requestedFrameRate, session.config.frameRate,
                        session.deliveryFrameRate);
            }
        }
        LogInfo(L"OpenXR: client attached, delivering {} fps", session.deliveryFrameRate);
    }

    const auto interval = std::chrono::nanoseconds(kNsPerSecond /
                                                   std::max<uint32_t>(session.deliveryFrameRate, 1));

    const auto now = std::chrono::steady_clock::now();

    // No sleeping here. A real OpenXR runtime blocks in xrWaitFrame to align the application
    // with its compositor's vsync - but there is no compositor on this side. Frames go to an
    // encoder, which applies its own backpressure, so blocking buys nothing.
    //
    // It costs a great deal. The application calls xrWaitFrame from its render loop, and
    // PrimedGun's swapchain interface exposes AcquireGraphicsQueueLock and
    // WaitForPendingFrameFinalization, so that loop holds a graphics queue lock across the
    // call. Sleeping inside it stalls GPU submission and presentation; a console app that
    // stops presenting has its display pipeline torn down by the watchdog, which is a hard
    // lock that takes the HDMI output with it.
    //
    // That matches every measurement: it happened with encoding compiled out, and again at a
    // tenth of the rendering load, because it was never about how much work was being done.
    session.nextFrameDue = now + interval;

    {
        std::lock_guard lock(session.frameMutex);
        session.frame.frameIndex = session.frameIndex;
        session.frame.timeSeconds =
            std::chrono::duration<double>(now - session.sessionStart).count();
        session.link.PollPose(session.frame);
    }

    // Answer any keyframe request the client made while assembling recent frames, and let
    // the request rate steer the bitrate.
    //
    // One request produces one keyframe on both eyes: they decode independently, and
    // recovering only the eye that reported loss leaves the headset showing one clean image
    // and one smeared one, which is worse to look at than two equally stale ones.
    if (session.link.ConsumeKeyframeRequest())
    {
        for (auto& encoder : session.encoders)
        {
            if (encoder)
            {
                encoder->RequestKeyframe();
            }
        }
        session.keyframeRequests++;
    }

    AdaptBitrate(session);


    session.predictedDisplayTime = NowXrTime() + std::chrono::nanoseconds(interval).count();

    state.predictedDisplayTime = session.predictedDisplayTime;
    state.predictedDisplayPeriod = std::chrono::nanoseconds(interval).count();
    // Rendering continues with no client attached. The alternative - reporting
    // shouldRender false until someone connects - means the first frames after a client
    // appears come from a loop that has not run in a while, and applications differ wildly
    // in how gracefully they resume.
    state.shouldRender = XR_TRUE;
    return XR_SUCCESS;
}

/**
 * The format to stage a swapchain image in before converting it to NV12.
 *
 * Two things constrain this. CopySubresourceRegion will not convert between formats, so it
 * has to stay in the swapchain's typeless family - which rules out picking one fixed format
 * and hoping the application chose the same one. And the video processor rejects an sRGB
 * *view* with a bare E_INVALIDARG, so the sRGB flavour has to be dropped. Both hold at
 * once because _UNORM and _UNORM_SRGB share a family: the copy is legal and the view is
 * accepted. The bits are identical either way; only their interpretation differs, and
 * nothing between here and the encoder interprets them.
 */
DXGI_FORMAT StagingFormatFor(DXGI_FORMAT swapchainFormat)
{
    switch (swapchainFormat)
    {
    // sRGB is stripped because the video processor rejects it outright, and the bytes are
    // already encoded - reinterpreting them as UNORM hands the encoder exactly what it should
    // compress, with no second gamma pass.
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;

    // TYPELESS must be resolved to a concrete type for the same reason, and it now reaches
    // here routinely: swapchains created with MUTABLE_FORMAT are backed by TYPELESS textures,
    // so the source format handed to EnsureStaging is TYPELESS rather than the format the
    // application declared. CreateVideoProcessorInputView cannot infer a layout from a
    // typeless resource and fails with E_INVALIDARG.
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;

    default:
        return swapchainFormat;
    }
}

/** Creates the staging texture for one eye, matching the application's swapchain format. */
bool EnsureStaging(Session& session, size_t eye, DXGI_FORMAT format)
{
    if (session.staging[eye])
    {
        return true;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = session.config.width;
    desc.Height = session.config.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = StagingFormatFor(format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(session.device->CreateTexture2D(&desc, nullptr, session.staging[eye].put())))
    {
        LogError(L"OpenXR: staging texture creation failed for eye {}", eye);
        return false;
    }
    return true;
}

/**
 * Copies one eye out of whatever the application rendered into, and encodes it.
 *
 * The submitted subimage may be a slice of an array swapchain, a rect inside a larger
 * texture, or the whole thing. Copying into a fixed staging texture collapses all three
 * into one path and guarantees the encoder always sees exactly the size it was configured
 * for, which it is not forgiving about.
 */
void EncodeEye(Session& session, size_t eye, const XrSwapchainSubImage& subImage,
               int64_t timestampHns, uint64_t captureTimeUs, uint32_t poseSequence)
{
    auto* swapchain = reinterpret_cast<Swapchain*>(subImage.swapchain);
    if (swapchain == nullptr || swapchain->images.empty())
    {
        return;
    }

    // The application released the image before calling xrEndFrame, so the most recently
    // released index is the one holding this frame.
    const uint32_t index = swapchain->acquiredIndex % swapchain->images.size();
    ID3D11Texture2D* source = swapchain->images[index].get();

    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (!EnsureStaging(session, eye, sourceDesc.Format))
    {
        return;
    }

    const uint32_t width = std::min<uint32_t>(static_cast<uint32_t>(subImage.imageRect.extent.width),
                                              session.config.width);
    const uint32_t height = std::min<uint32_t>(
        static_cast<uint32_t>(subImage.imageRect.extent.height), session.config.height);

    if ((width != session.config.width || height != session.config.height) &&
        !session.warnedRectMismatch)
    {
        session.warnedRectMismatch = true;
        LogInfo(L"OpenXR: submitted rect {}x{} differs from encoder frame {}x{}; the "
                L"difference is left blank",
                subImage.imageRect.extent.width, subImage.imageRect.extent.height,
                session.config.width, session.config.height);
    }

    D3D11_BOX box{};
    box.left = static_cast<UINT>(subImage.imageRect.offset.x);
    box.top = static_cast<UINT>(subImage.imageRect.offset.y);
    box.front = 0;
    box.right = box.left + width;
    box.bottom = box.top + height;
    box.back = 1;

    const UINT sourceSubresource =
        D3D11CalcSubresource(0, subImage.imageArrayIndex, swapchain->info.mipCount);

    session.context->CopySubresourceRegion(session.staging[eye].get(), 0, 0, 0, 0, source,
                                           sourceSubresource, &box);

    ID3D11Texture2D* nv12 = session.converters[eye].Convert(session.staging[eye].get());
    session.encoders[eye]->Submit(nv12, timestampHns, static_cast<uint32_t>(session.frameIndex),
                                  captureTimeUs, poseSequence);
}

/**
 * Asks the GPU whether it is still alive, and gives up cleanly if it is not.
 *
 * A hung GPU on a console takes the whole machine with it - the display dies, Device Portal
 * goes with it, and there is no crash dump because nothing crashed. GetDeviceRemovedReason is
 * the one place the driver states what actually went wrong, and nothing here was asking.
 *
 * Two things come out of this. The reason code is a real diagnosis rather than an inference:
 * DEVICE_HUNG means our commands hung it, INVALID_CALL means the application asked for
 * something illegal, DRIVER_INTERNAL_ERROR means neither. And exiting the process the moment
 * it is seen gives the driver a chance to reclaim the device before the system watchdog gives
 * up on it, which is the difference between an app that closes and a console that needs a
 * mode round trip to recover.
 */
void CheckDeviceHealth(Session& session)
{
    if (!session.device)
    {
        return;
    }

    const HRESULT reason = session.device->GetDeviceRemovedReason();
    if (SUCCEEDED(reason))
    {
        return;
    }

    const wchar_t* description = L"unknown";
    switch (reason)
    {
    case DXGI_ERROR_DEVICE_HUNG:
        description = L"DEVICE_HUNG - the commands submitted by this app hung the GPU";
        break;
    case DXGI_ERROR_DEVICE_REMOVED:
        description = L"DEVICE_REMOVED";
        break;
    case DXGI_ERROR_DEVICE_RESET:
        description = L"DEVICE_RESET - the GPU was reset out from under us";
        break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
        description = L"DRIVER_INTERNAL_ERROR - a fault inside the driver itself";
        break;
    case DXGI_ERROR_INVALID_CALL:
        description = L"INVALID_CALL - this app made an illegal D3D call";
        break;
    default:
        break;
    }

    LogError(L"GPU DEVICE LOST after {} frames: 0x{:08X} ({})", session.frameIndex,
             static_cast<uint32_t>(reason), description);
    LogError(L"Exiting now rather than waiting for the system watchdog, which takes the "
             L"display with it.");

    // Not an exception and not a return: the calling thread is the render thread, and letting
    // it continue submitting to a dead device is what turns this into a console lock-up.
    std::_Exit(3);
}

XrResult SubmitFrameImpl(Session& session, const XrFrameEndInfo& endInfo)
{
    session.lastFrameTick.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count(),
                                std::memory_order_relaxed);
    CheckDeviceHealth(session);

    const XrCompositionLayerProjection* projection = nullptr;
    for (uint32_t i = 0; i < endInfo.layerCount; ++i)
    {
        const XrCompositionLayerBaseHeader* layer = endInfo.layers[i];
        if (layer != nullptr && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
        {
            projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            break;
        }
    }

    // Quad and other layer types are accepted and ignored rather than rejected. There is no
    // compositor on this side to blend them into - the encoder takes one image per eye - and
    // failing xrEndFrame over a layer the application considers optional would stop the
    // stream dead.
    if (projection == nullptr || projection->viewCount < 2)
    {
        // Silence here is indistinguishable from a black headset caused by anything else, so
        // say it once. An application that submits no projection layer is telling us it has
        // nothing to display, and no amount of encoder or network health will change that.
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            LogWarn(L"xrEndFrame: no usable projection layer (layerCount={}, viewCount={}). "
                    L"Nothing will be encoded while this is the case.",
                    endInfo.layerCount, projection != nullptr ? projection->viewCount : 0);
        }
        ++session.frameIndex;
        // Counted before returning, so "the application submitted nothing" is still visible
        // in the periodic report rather than looking like the frame loop stopped.
        ++session.framesWithoutLayer;
        return XR_SUCCESS;
    }

    const int64_t timestampHns = static_cast<int64_t>(session.frameIndex) * kHnsPerSecond /
                                 std::max<uint32_t>(session.deliveryFrameRate, 1);
    const auto captureTimeUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    uint32_t poseSequence = 0;
    {
        std::lock_guard lock(session.frameMutex);
        poseSequence = session.frame.poseSequence;
    }

    if (!session.encodeDisabled)
    {
        for (size_t eye = 0; eye < 2; ++eye)
        {
            EncodeEye(session, eye, projection->views[eye].subImage, timestampHns,
                      captureTimeUs, poseSequence);
        }
    }

    ++session.frameIndex;

    // Periodic proof of life. Without this the log goes silent after setup, and "the headset
    // is black" is indistinguishable from the application never submitting a frame, the
    // encoder swallowing them, or the socket dropping them. Each of those is a different bug
    // and they are not separable after the fact.
    //
    // Dense at the start, sparse afterwards: the console has been locking within seconds, and
    // a report that only arrives every 300 frames never arrives at all if the machine dies at
    // frame 40. Every line is flushed, so whatever is last in the file is the last thing that
    // happened before it went.
    const bool report = session.frameIndex <= 10 || (session.frameIndex < 300 && session.frameIndex % 30 == 0) ||
                        session.frameIndex % 300 == 0;
    if (report)
    {
        const auto& left = session.encoders[0]->GetStats();
        const auto& right = session.encoders[1]->GetStats();
        const auto& sink = session.sinks[0].GetStats();
        // Memory alongside the frame counters, because the console dies further into the
        // game each time something unbounded is removed - which is what running out of memory
        // looks like from the outside. Measuring it settles whether that is actually the
        // mechanism, rather than reading eight thousand lines of analysis code hoping to spot
        // the container that grows.
        uint64_t usedMb = 0;
        uint64_t limitMb = 0;
        try
        {
            using winrt::Windows::System::MemoryManager;
            usedMb = MemoryManager::AppMemoryUsage() / (1024 * 1024);
            limitMb = MemoryManager::AppMemoryUsageLimit() / (1024 * 1024);
        }
        catch (...)
        {
            // Not fatal; the frame counters are still worth having.
        }
        // GPU memory, separately, because AppMemoryUsage above does not include it and on a
        // console the two share one pool. An app can sit at a third of its CPU allowance while
        // the GPU budget is full, and a failed texture allocation there is a crash with no
        // warning anywhere in the numbers we were already printing. Budget is what the OS is
        // currently willing to give us, which is the figure that matters - not the adapter's
        // total.
        uint64_t gpuUsedMb = 0;
        uint64_t gpuBudgetMb = 0;
        if (session.device)
        {
            winrt::com_ptr<IDXGIDevice> dxgiDevice;
            if (SUCCEEDED(session.device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()))))
            {
                winrt::com_ptr<IDXGIAdapter> adapter;
                if (SUCCEEDED(dxgiDevice->GetAdapter(adapter.put())))
                {
                    auto adapter3 = adapter.try_as<IDXGIAdapter3>();
                    if (adapter3)
                    {
                        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
                        {
                            gpuUsedMb = info.CurrentUsage / (1024 * 1024);
                            gpuBudgetMb = info.Budget / (1024 * 1024);
                        }
                    }
                }
            }
        }

        LogInfo(L"memory: {} MB used of {} MB limit | gpu: {} MB used of {} MB budget", usedMb,
                limitMb, gpuUsedMb, gpuBudgetMb);

        LogInfo(L"frames: submitted={} noLayer={} | encoded L={} R={} dropped L={} R={} | sent={} "
                L"fragments={} bytes={} failures={} | poses={} rejected={} | keyframeReqs={}",
                session.frameIndex, session.framesWithoutLayer, left.framesEncoded,
                right.framesEncoded, left.framesDropped,
                right.framesDropped, sink.framesSent, sink.fragmentsSent, sink.bytesSent,
                sink.sendFailures, session.link.PosePacketsReceived(),
                session.link.PosesRejected(), session.keyframeRequests);
    }
    return XR_SUCCESS;
}

} // namespace

XrResult SessionStartPipeline(Session& session)
{
    return Guard(L"pipeline startup", [&] { return StartPipelineImpl(session); });
}

XrResult SessionWaitFrame(Session& session, XrFrameState& state)
{
    return Guard(L"xrWaitFrame", [&] { return WaitFrameImpl(session, state); });
}

XrResult SessionSubmitFrame(Session& session, const XrFrameEndInfo& endInfo)
{
    return Guard(L"xrEndFrame", [&] { return SubmitFrameImpl(session, endInfo); });
}

void SessionShutdown(Session& session)
{
    // Shutdown runs on the way out of xrDestroySession and xrEndSession, where there is
    // nothing useful to do with a failure and nowhere for an exception to go.
    // Stop the watchdog first: it reads session state, and everything below destroys it.
    session.watchdogRunning.store(false, std::memory_order_release);
    if (session.watchdog.joinable())
    {
        session.watchdog.join();
    }

    Guard(L"session shutdown", [&]() -> XrResult {
        for (auto& encoder : session.encoders)
        {
            if (encoder)
            {
                encoder->Drain();
                encoder->Shutdown();
                encoder.reset();
            }
        }
        for (auto& sink : session.sinks)
        {
            sink.Shutdown();
        }
        session.link.Stop();
        session.pipelineReady = false;
        return XR_SUCCESS;
    });
}

} // namespace xvr::xr
