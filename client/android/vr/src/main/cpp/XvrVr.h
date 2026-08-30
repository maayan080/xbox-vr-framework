#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <android/log.h>

#include <string>
#include <vector>

// jni.h must precede openxr_platform.h: the Android platform bindings reference jobject
// and JavaVM directly, and the header does not pull them in itself.
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

struct android_app;

#define XVR_LOG(...) __android_log_print(ANDROID_LOG_INFO, "xvrvr", __VA_ARGS__)
#define XVR_WARN(...) __android_log_print(ANDROID_LOG_WARN, "xvrvr", __VA_ARGS__)
#define XVR_ERR(...) __android_log_print(ANDROID_LOG_ERROR, "xvrvr", __VA_ARGS__)

namespace xvr {

/** What the headset should show, when there is no video to show yet. */
enum class Status
{
    WaitingForHost,   // nothing arriving
    ReceivingNoKeyframe, // packets arriving, decoder cannot start yet
    Decoding,         // decoder running but no texture delivered yet
    Streaming,        // video on screen
};

/** GL resources and drawing. Owns the eye textures fed by MediaCodec. */
class GlRenderer
{
public:
    bool Initialize();
    void Shutdown();

    /**
     * External OES texture ids, one per eye, created on the render thread's context.
     * These are handed to Java to wrap in SurfaceTextures, which MediaCodec renders into.
     */
    GLuint EyeTexture(int eye) const { return m_eyeTextures[eye]; }

    /**
     * Draws one eye into the currently bound framebuffer.
     *
     * `viewProjection` is the full transform for this eye, so the screen is a real object
     * at a fixed point in space. Drawing without it - filling the view with a flat image
     * per eye - gives the eyes nothing to converge on and is painful within seconds.
     */
    void DrawEye(int eye, int width, int height, Status status, float timeSeconds,
                 const float* viewProjection);

    /** Timewarp inputs for one eye. Set every frame before drawing. */
    struct Warp
    {
        // Rotation from current eye space into the space the frame was rendered in.
        float reprojection[9]{ 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        // Current eye orientation (eye -> world) and world -> render eye space.
        float currentRotation[9]{ 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        float renderRotationInv[9]{ 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        // Eye positions in the client's tracking space, metres.
        float currentPosition[3]{};
        float renderPosition[3]{};
        // tan of the frustum half-angles: left, right, up, down.
        float currentFovTan[4]{ -1, 1, 1, -1 };
        float renderFovTan[4]{ -1, 1, 1, -1 };
        bool valid = false;
    };

    void SetReprojection(int eye, const Warp& warp);

private:
    GLuint CompileProgram(const char* vertexSource, const char* fragmentSource);

    GLuint m_eyeTextures[2]{ 0, 0 };
    GLuint m_videoProgram = 0;
    GLuint m_patternProgram = 0;
    GLuint m_vertexArray = 0;
    GLint m_patternColourLocation = -1;
    GLint m_patternTimeLocation = -1;
    GLint m_patternMvpLocation = -1;
    GLint m_videoTextureLocation = -1;
    GLint m_videoReprojectionLocation = -1;
    GLint m_videoCurrentRotationLocation = -1;
    GLint m_videoRenderRotationInvLocation = -1;
    GLint m_videoCurrentPositionLocation = -1;
    GLint m_videoRenderPositionLocation = -1;
    GLint m_videoFocusDepthLocation = -1;
    GLint m_videoCurrentFovLocation = -1;
    GLint m_videoRenderFovLocation = -1;

    Warp m_reprojection[2];
};

/** OpenXR instance, session, swapchains and frame loop. */
class XrApp
{
public:
    bool Initialize(android_app* app);
    void Shutdown();

    /** Pumps events and renders one frame. Returns false when the app should exit. */
    bool Tick();

    bool IsSessionRunning() const { return m_sessionRunning; }
    GlRenderer& Renderer() { return m_renderer; }

    /** Called from the app layer so the renderer can show progress before video works. */
    void SetStatus(Status status) { m_status = status; }

    /** Invoked on the render thread each frame, so SurfaceTextures can be updated with
     *  the GL context current - which is the only thread where that is legal. */
    void SetFrameCallback(void (*callback)(void*), void* context)
    {
        m_frameCallback = callback;
        m_frameCallbackContext = context;
    }

    /**
     * Invoked with the viewpoint the runtime just reported, so it can be sent to the host.
     * Called before rendering, because the host needs it as early as possible.
     *
     * `pose` is 7 floats (position xyz, orientation xyzw); `eyes` is 22 floats
     * (per eye: position[3], orientation[4], fov[4]).
     */
    void SetPoseCallback(void (*callback)(const float*, const float*, void*), void* context)
    {
        m_poseCallback = callback;
        m_poseCallbackContext = context;
    }

    /**
     * Controller state for one hand, laid out to match the wire format so it can be sent
     * without translation. 19 floats: buttons (bit-packed into a float), grip position and
     * orientation, aim position and orientation, trigger, squeeze, thumbstick.
     */
    static constexpr int kControllerFloats = 19;

    /** Invoked with both hands' state each frame: 2 x kControllerFloats. */
    void SetControllerCallback(void (*callback)(const float*, void*), void* context)
    {
        m_controllerCallback = callback;
        m_controllerCallbackContext = context;
    }

private:
    bool CreateInstance(android_app* app);
    bool CreateSessionAndSwapchains();
    bool CreateInputActions();
    void UpdateControllers(XrTime predictedDisplayTime);
    void PollEvents();
    void RenderFrame(XrTime predictedDisplayTime);

    // OpenXR's action system, one action per input rather than per controller model. The
    // runtime maps these onto whatever hardware is present, which is why bindings are
    // "suggested" rather than assigned.
    struct InputActions
    {
        XrActionSet actionSet = XR_NULL_HANDLE;
        XrAction gripPose = XR_NULL_HANDLE;
        XrAction aimPose = XR_NULL_HANDLE;
        XrAction trigger = XR_NULL_HANDLE;
        XrAction squeeze = XR_NULL_HANDLE;
        XrAction thumbstick = XR_NULL_HANDLE;
        XrAction primaryClick = XR_NULL_HANDLE;
        XrAction secondaryClick = XR_NULL_HANDLE;
        XrAction menuClick = XR_NULL_HANDLE;
        XrAction thumbstickClick = XR_NULL_HANDLE;

        XrPath handPaths[2]{ XR_NULL_PATH, XR_NULL_PATH };
        XrSpace gripSpaces[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };
        XrSpace aimSpaces[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };
    };

    InputActions m_input;
    float m_controllerState[2 * kControllerFloats]{};

    struct SwapchainInfo
    {
        XrSwapchain handle = XR_NULL_HANDLE;
        int32_t width = 0;
        int32_t height = 0;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
    };

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_space = XR_NULL_HANDLE;
    XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;

    std::vector<XrViewConfigurationView> m_viewConfigs;
    std::vector<SwapchainInfo> m_swapchains;
    std::vector<XrView> m_views;

    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLContext m_eglContext = EGL_NO_CONTEXT;
    EGLSurface m_eglSurface = EGL_NO_SURFACE;
    GLuint m_framebuffer = 0;

    GlRenderer m_renderer;
    Status m_status = Status::WaitingForHost;

    bool m_sessionRunning = false;
    bool m_exitRequested = false;
    float m_startTimeSeconds = 0.0f;

    void (*m_frameCallback)(void*) = nullptr;
    void* m_frameCallbackContext = nullptr;

    void (*m_poseCallback)(const float*, const float*, void*) = nullptr;
    void* m_poseCallbackContext = nullptr;

    void (*m_controllerCallback)(const float*, void*) = nullptr;
    void* m_controllerCallbackContext = nullptr;
};

} // namespace xvr
