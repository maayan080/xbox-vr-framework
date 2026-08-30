#pragma once

// An OpenXR runtime backed by the streaming pipeline.
//
// An application links this instead of the Khronos loader and its existing OpenXR code
// works unchanged: xrLocateViews hands back the headset's pose as reported over the
// network, and xrEndFrame takes the eye textures it just rendered and feeds them to the
// encoder. The application never learns that the display is a Quest on the other side of
// a Wi-Fi link.
//
// Supported today: D3D11 graphics binding, primary stereo view configuration, LOCAL /
// STAGE / VIEW reference spaces, the frame loop, and the action system for controllers.
// Everything else returns XR_ERROR_FUNCTION_UNSUPPORTED rather than pretending.

#include "xvr/ClientLink.h"
#include "xvr/Constraints.h"
#include "xvr/NV12Converter.h"
#include "xvr/StereoRenderer.h"
#include "xvr/UdpFrameSink.h"
#include "xvr/VideoEncoder.h"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <d3d11_4.h>

#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace xvr::xr {

// OpenXR handles are opaque pointers to these.
struct Instance;
struct Session;
struct Space;
struct Swapchain;
struct ActionSet;
struct Action;

/** A path string interned to an XrPath, as the action system requires. */
class PathTable
{
public:
    XrPath Intern(const std::string& text);
    bool Lookup(XrPath path, std::string& text) const;

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_paths;
};

struct Action
{
    ActionSet* actionSet = nullptr;
    XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::string name;
    std::string localizedName;
    std::vector<XrPath> subactionPaths;

    // Which control this action was bound to, resolved from the suggested bindings. The
    // runtime decides this in real OpenXR, and here it is decided by matching the binding
    // path against the controller state the client sends.
    enum class Binding
    {
        None,
        GripPose,
        AimPose,
        Trigger,
        Squeeze,
        Thumbstick,
        PrimaryClick,
        SecondaryClick,
        MenuClick,
        ThumbstickClick,
    };

    // Per hand, because the same action is usually suggested for both.
    Binding bindings[2] = { Binding::None, Binding::None };
};

struct ActionSet
{
    Instance* instance = nullptr;
    std::string name;
    std::string localizedName;
    uint32_t priority = 0;
    std::vector<std::unique_ptr<Action>> actions;
    bool attached = false;
};

struct Space
{
    Session* session = nullptr;
    XrReferenceSpaceType referenceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrPosef poseInSpace{ { 0, 0, 0, 1 }, { 0, 0, 0 } };

    // Set for action spaces rather than reference spaces.
    Action* action = nullptr;
    XrPath subactionPath = XR_NULL_PATH;
    bool isActionSpace = false;
};

struct Swapchain
{
    Session* session = nullptr;
    XrSwapchainCreateInfo info{};
    std::vector<winrt::com_ptr<ID3D11Texture2D>> images;

    uint32_t acquiredIndex = 0;
    bool imageAcquired = false;
    bool imageWaited = false;
};

struct Session
{
    Instance* instance = nullptr;
    XrSessionState state = XR_SESSION_STATE_IDLE;
    bool running = false;
    bool exiting = false;

    // The application's device. Swapchain textures and the encoder both live on it, which
    // avoids any cross-device sharing of the frames being streamed.
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;

    std::vector<std::unique_ptr<Space>> spaces;
    std::vector<std::unique_ptr<Swapchain>> swapchains;
    std::vector<ActionSet*> attachedActionSets;

    // Streaming side.
    ClientLink link;
    std::array<UdpFrameSink, 2> sinks;
    std::array<std::unique_ptr<VideoEncoder>, 2> encoders;
    std::array<NV12Converter, 2> converters;

    // The application renders wherever it likes inside its own swapchain; the encoder
    // needs one texture of exactly the configured size. Everything submitted lands here
    // first, which also makes array slices and sub-rects a single uniform case.
    std::array<winrt::com_ptr<ID3D11Texture2D>, 2> staging;
    bool warnedRectMismatch = false;

    bool pipelineReady = false;
    bool clientConnected = false;
    std::string clientAddress;

    // Set by the xvr-no-encode.txt marker file. Runs the session but skips NV12 conversion,
    // encoding and transmission, so the application's rendering can be exercised without the
    // parts that touch the video hardware.
    bool encodeDisabled = false;

    // Frames where the application called xrEndFrame with nothing to display. Counted so the
    // periodic report distinguishes "not rendering" from "rendering but not reaching us".
    uint64_t framesWithoutLayer = 0;
    // Keyframes forced because the client could not assemble a frame. A rising count here
    // is packet loss, not an encoder problem.
    uint64_t keyframeRequests = 0;

    // Adaptive bitrate state. The link, not the encoder, decides what fits.
    uint32_t currentBitrateBps = 0;
    uint64_t keyframeRequestsAtLastCheck = 0;
    std::chrono::steady_clock::time_point lastBitrateCheck{};

    // Watchdog. The device-lost check only runs if a thread is alive to run it, and the
    // failure being chased stops the render thread dead - so a separate thread watches the
    // frame clock and gives up if it stops advancing.
    std::thread watchdog;
    std::atomic<bool> watchdogRunning{ false };
    std::atomic<int64_t> lastFrameTick{ 0 };

    // kDefaultPerEyeConfig, not kMaxResolutionPerEyeConfig.
    //
    // The max constant is what the encoder will just barely accept - 1920x1088, and only at
    // 60 fps, because 72 would exceed the macroblock throughput ceiling. Using it as the
    // default meant the headset ran at 60 on a 72 Hz display, so every sixth frame was shown
    // twice and every head turn juddered.
    //
    // The default constant exists for exactly this and was simply never used here: slightly
    // fewer pixels, at the rate the display actually refreshes.
    //
    // XVR_LOW_RES asks for a quarter of the pixels, to separate encoder cost from the GPU
    // cost of rendering two eyes while also emulating a console.
#ifdef XVR_LOW_RES
    FrameConfig config{ 960, 544, 72, 12'000'000 };
#else
    FrameConfig config = kDefaultPerEyeConfig;
#endif
    uint32_t deliveryFrameRate = 72;

    // Frame loop bookkeeping.
    uint64_t frameIndex = 0;
    bool frameBegun = false;
    std::chrono::steady_clock::time_point sessionStart;
    std::chrono::steady_clock::time_point nextFrameDue;

    // Latest pose from the client, refreshed in xrWaitFrame.
    FrameContext frame;
    std::mutex frameMutex;

    XrTime predictedDisplayTime = 0;

    // Input is sampled at xrSyncActions and held until the next one, because OpenXR
    // guarantees action state does not change underneath an application mid-frame.
    // The previous snapshot is kept so changedSinceLastSync can be answered honestly
    // rather than always reporting true.
    FrameContext actionFrame;
    FrameContext actionFramePrev;
    XrTime actionSyncTime = 0;
    bool actionsAttached = false;
};

struct Instance
{
    std::string applicationName;
    std::vector<std::string> enabledExtensions;
    PathTable paths;
    std::vector<std::unique_ptr<ActionSet>> actionSets;
    std::unique_ptr<Session> session;

    // Queued for xrPollEvent, which is how an application learns the session is ready.
    std::vector<XrEventDataBuffer> events;
    std::mutex eventMutex;

    void QueueSessionState(XrSession session, XrSessionState state);
};

// --- Pose maths ------------------------------------------------------------------------
//
// OpenXR poses compose and invert constantly (locating a space relative to another space is
// exactly that), so the operations are here rather than open-coded at each call site.

XrPosef ToXrPose(const float position[3], const float orientation[4]);
XrPosef IdentityPose();
XrPosef Multiply(const XrPosef& parent, const XrPosef& child);
XrPosef Invert(const XrPosef& pose);

/** Steady clock in OpenXR's nanosecond time base. */
XrTime NowXrTime();

// --- Session internals -----------------------------------------------------------------

/** Brings up the encoders, sinks and discovery socket. Called on the first frame. */
XrResult SessionStartPipeline(Session& session);

/** Blocks until the next frame is due, refreshes the pose, fills `state`. */
XrResult SessionWaitFrame(Session& session, XrFrameState& state);

/** Takes the submitted projection layer, converts each eye and encodes it. */
XrResult SessionSubmitFrame(Session& session, const XrFrameEndInfo& endInfo);

void SessionShutdown(Session& session);

/** Resolves a suggested binding path onto the controller state the client sends. */
Action::Binding BindingFromPath(const std::string& path);

/** Which hand a path refers to: 0 left, 1 right, -1 if it names neither. */
int HandFromPath(const std::string& path);

} // namespace xvr::xr
