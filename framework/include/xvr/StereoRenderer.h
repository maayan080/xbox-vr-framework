#pragma once

// The framework's core content contract.
//
// Everything the host needs from the application is "render one stereo frame".
// The framework owns the device, the render targets, the encoder and (later) the
// network transport; the application owns only what appears inside the eye views.
//
// Nothing in this header may assume anything about the content being rendered.

#include <cstdint>

#include <d3d11.h>

namespace xvr {

enum class Eye : uint32_t
{
    Left = 0,
    Right = 1,
};

// One eye's viewpoint as reported by the headset, in the client's tracking space.
struct EyeViewpoint
{
    float position[3]{};
    float orientation[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    // Frustum half-angles in radians: left, right, up, down. Left and down are negative.
    // Asymmetric on real headsets, so this must be used rather than assumed.
    float fov[4]{ -0.8f, 0.8f, 0.8f, -0.8f };
};

// One controller as reported by the client, mirroring OpenXR's input actions so that a
// future OpenXR surface can answer xrGetActionState* directly from this.
struct ControllerInput
{
    bool active = false;

    // Grip is where the hand is; aim is where the controller points. Both are carried
    // because deriving one from the other needs device-specific geometry.
    float gripPosition[3]{};
    float gripOrientation[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    float aimPosition[3]{};
    float aimOrientation[4]{ 0.0f, 0.0f, 0.0f, 1.0f };

    float trigger = 0.0f;  // 0..1
    float squeeze = 0.0f;  // 0..1
    float thumbstick[2]{}; // -1..1

    // Bitmask of xvr::net::ControllerButtons.
    uint32_t buttons = 0;
};

// Per-frame state handed to the renderer.
struct FrameContext
{
    uint64_t frameIndex = 0;
    double timeSeconds = 0.0;

    // False until the client has sent a pose. A renderer must still produce a sensible
    // image in that case - the host streams before anyone is looking, and a black or
    // wildly wrong frame makes it impossible to tell a pose problem from a video one.
    bool poseValid = false;

    float headPosition[3]{};
    float headOrientation[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    EyeViewpoint eyes[2];

    // Index 0 is left, 1 is right. Check `active` before using either.
    ControllerInput controllers[2];

    // Which pose this frame is rendered against. Echoed to the client so it can reproject
    // the finished image against a newer pose rather than displaying a stale one.
    uint32_t poseSequence = 0;

    // How old the pose was when this frame began rendering. This is a direct component of
    // motion-to-photon latency, so it is measured rather than assumed.
    double poseAgeMs = 0.0;
};

// The surface for a single eye. The framework may back both eyes with one texture
// and hand out differing viewports, so the renderer must respect `viewport` rather
// than assuming it owns the whole target.
struct EyeView
{
    ID3D11RenderTargetView* renderTarget = nullptr;
    ID3D11DepthStencilView* depthStencil = nullptr;
    D3D11_VIEWPORT viewport{};
    uint32_t width = 0;
    uint32_t height = 0;
};

class IStereoRenderer
{
public:
    virtual ~IStereoRenderer() = default;

    // Called once, before the first frame, on the framework's device.
    virtual void Initialize(ID3D11Device* device, ID3D11DeviceContext* context) = 0;

    // Called twice per frame, once per eye. The framework has already cleared the
    // target and bound nothing; the renderer sets up its own pipeline state.
    virtual void RenderEye(const FrameContext& frame, Eye eye, const EyeView& view) = 0;
};

} // namespace xvr
