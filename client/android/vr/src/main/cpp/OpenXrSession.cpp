#include "XvrVr.h"

#include <android_native_app_glue.h>

#include <cmath>
#include <cstring>

namespace xvr {
namespace {

bool Check(XrResult result, const char* what)
{
    if (XR_SUCCEEDED(result))
    {
        return true;
    }
    XVR_ERR("%s failed: %d", what, static_cast<int>(result));
    return false;
}

// Column-major 4x4, matching what glUniformMatrix4fv expects with transpose = GL_FALSE.

void MultiplyMatrices(const float* a, const float* b, float* out)
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            float sum = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                sum += a[i * 4 + row] * b[column * 4 + i];
            }
            out[column * 4 + row] = sum;
        }
    }
}

/**
 * Asymmetric projection from the runtime's field of view.
 *
 * A headset's per-eye frustum is not centred - the outer edge extends further than the
 * inner one - so a symmetric perspective matrix would misplace everything and break
 * fusion between the eyes.
 */
void ProjectionFromFov(const XrFovf& fov, float nearZ, float farZ, float* out)
{
    const float tanLeft = std::tan(fov.angleLeft);
    const float tanRight = std::tan(fov.angleRight);
    const float tanDown = std::tan(fov.angleDown);
    const float tanUp = std::tan(fov.angleUp);

    const float tanWidth = tanRight - tanLeft;
    const float tanHeight = tanUp - tanDown;

    for (int i = 0; i < 16; ++i)
    {
        out[i] = 0.0f;
    }

    out[0] = 2.0f / tanWidth;
    out[5] = 2.0f / tanHeight;
    out[8] = (tanRight + tanLeft) / tanWidth;
    out[9] = (tanUp + tanDown) / tanHeight;
    out[10] = -(farZ + nearZ) / (farZ - nearZ);
    out[11] = -1.0f;
    out[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

/** View matrix from an eye pose: the inverse of that pose's rotation and translation. */
void ViewFromPose(const XrPosef& pose, float* out)
{
    const XrQuaternionf& q = pose.orientation;

    const float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    const float xx = q.x * x2, xy = q.x * y2, xz = q.x * z2;
    const float yy = q.y * y2, yz = q.y * z2, zz = q.z * z2;
    const float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;

    // Rotation transposed, which for an orthonormal rotation is its inverse.
    float rotation[16] = {
        1.0f - (yy + zz), xy - wz,          xz + wy,          0.0f,
        xy + wz,          1.0f - (xx + zz), yz - wx,          0.0f,
        xz - wy,          yz + wx,          1.0f - (xx + yy), 0.0f,
        0.0f,             0.0f,             0.0f,             1.0f
    };

    float translation[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        -pose.position.x, -pose.position.y, -pose.position.z, 1.0f
    };

    MultiplyMatrices(rotation, translation, out);
}

} // namespace

bool XrApp::CreateInstance(android_app* app)
{
    // On Android the loader must be initialised with the VM and activity before anything
    // else, including xrEnumerate* calls. Skipping this fails in confusing ways later.
    PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
    if (XR_SUCCEEDED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                           reinterpret_cast<PFN_xrVoidFunction*>(
                                               &xrInitializeLoaderKHR))) &&
        xrInitializeLoaderKHR != nullptr)
    {
        XrLoaderInitInfoAndroidKHR loaderInfo{ XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
        loaderInfo.applicationVM = app->activity->vm;
        loaderInfo.applicationContext = app->activity->clazz;
        xrInitializeLoaderKHR(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo));
        XVR_LOG("loader initialised");
    }
    else
    {
        XVR_WARN("xrInitializeLoaderKHR unavailable; continuing");
    }

    const char* extensions[] = {
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    };

    XrInstanceCreateInfoAndroidKHR androidInfo{ XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    androidInfo.applicationVM = app->activity->vm;
    androidInfo.applicationActivity = app->activity->clazz;

    XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
    createInfo.next = &androidInfo;
    createInfo.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
    createInfo.enabledExtensionNames = extensions;
    std::strcpy(createInfo.applicationInfo.applicationName, "XVR Client");
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;

    if (!Check(xrCreateInstance(&createInfo, &m_instance), "xrCreateInstance"))
    {
        return false;
    }

    XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(xrGetSystem(m_instance, &systemInfo, &m_systemId), "xrGetSystem"))
    {
        return false;
    }

    XrSystemProperties properties{ XR_TYPE_SYSTEM_PROPERTIES };
    if (XR_SUCCEEDED(xrGetSystemProperties(m_instance, m_systemId, &properties)))
    {
        XVR_LOG("system: %s", properties.systemName);
    }

    return true;
}

bool XrApp::CreateSessionAndSwapchains()
{
    // Must be called before creating the session, even though the result is only used for
    // validation: some runtimes refuse session creation otherwise.
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
    xrGetInstanceProcAddr(m_instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements));
    if (getRequirements != nullptr)
    {
        XrGraphicsRequirementsOpenGLESKHR requirements{
            XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR
        };
        getRequirements(m_instance, m_systemId, &requirements);
    }

    // A pbuffer surface is enough: nothing is ever presented through EGL, only through
    // OpenXR's swapchains.
    m_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(m_eglDisplay, nullptr, nullptr);

    const EGLint configAttributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      0,
        EGL_NONE
    };

    EGLConfig config{};
    EGLint configCount = 0;
    eglChooseConfig(m_eglDisplay, configAttributes, &config, 1, &configCount);
    if (configCount == 0)
    {
        XVR_ERR("no suitable EGL config");
        return false;
    }

    const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    m_eglContext = eglCreateContext(m_eglDisplay, config, EGL_NO_CONTEXT, contextAttributes);

    const EGLint surfaceAttributes[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    m_eglSurface = eglCreatePbufferSurface(m_eglDisplay, config, surfaceAttributes);

    if (!eglMakeCurrent(m_eglDisplay, m_eglSurface, m_eglSurface, m_eglContext))
    {
        XVR_ERR("eglMakeCurrent failed");
        return false;
    }

    XrGraphicsBindingOpenGLESAndroidKHR binding{
        XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR
    };
    binding.display = m_eglDisplay;
    binding.config = config;
    binding.context = m_eglContext;

    XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
    sessionInfo.next = &binding;
    sessionInfo.systemId = m_systemId;

    if (!Check(xrCreateSession(m_instance, &sessionInfo, &m_session), "xrCreateSession"))
    {
        return false;
    }

    XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Check(xrCreateReferenceSpace(m_session, &spaceInfo, &m_space), "xrCreateReferenceSpace"))
    {
        return false;
    }

    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(m_instance, m_systemId,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
                                      nullptr);
    m_viewConfigs.resize(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    xrEnumerateViewConfigurationViews(m_instance, m_systemId,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount,
                                      &viewCount, m_viewConfigs.data());
    m_views.resize(viewCount, { XR_TYPE_VIEW });

    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data());

    // Prefer a plain RGBA8 target. An sRGB swapchain would have the runtime apply a second
    // conversion on top of the one the video already carries, which shows as washed out.
    int64_t chosenFormat = formats.empty() ? 0 : formats[0];
    for (int64_t format : formats)
    {
        if (format == GL_RGBA8)
        {
            chosenFormat = format;
            break;
        }
    }
    XVR_LOG("swapchain format 0x%llx", static_cast<unsigned long long>(chosenFormat));

    m_swapchains.resize(viewCount);
    for (uint32_t i = 0; i < viewCount; ++i)
    {
        XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        swapchainInfo.format = chosenFormat;
        swapchainInfo.sampleCount = 1;
        swapchainInfo.width = static_cast<int32_t>(m_viewConfigs[i].recommendedImageRectWidth);
        swapchainInfo.height = static_cast<int32_t>(m_viewConfigs[i].recommendedImageRectHeight);
        swapchainInfo.faceCount = 1;
        swapchainInfo.arraySize = 1;
        swapchainInfo.mipCount = 1;

        SwapchainInfo& swapchain = m_swapchains[i];
        swapchain.width = swapchainInfo.width;
        swapchain.height = swapchainInfo.height;

        if (!Check(xrCreateSwapchain(m_session, &swapchainInfo, &swapchain.handle),
                   "xrCreateSwapchain"))
        {
            return false;
        }

        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr);
        swapchain.images.resize(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR });
        xrEnumerateSwapchainImages(
            swapchain.handle, imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data()));

        XVR_LOG("eye %u swapchain %dx%d, %u images", i, swapchain.width, swapchain.height,
                imageCount);
    }

    glGenFramebuffers(1, &m_framebuffer);
    return true;
}

bool XrApp::Initialize(android_app* app)
{
    if (!CreateInstance(app))
    {
        return false;
    }
    if (!CreateSessionAndSwapchains())
    {
        return false;
    }
    if (!m_renderer.Initialize())
    {
        return false;
    }

    // Not fatal if this fails: a session without hands is far better than no session.
    CreateInputActions();

    XVR_LOG("OpenXR ready");
    return true;
}

void XrApp::PollEvents()
{
    XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };

    while (true)
    {
        event = { XR_TYPE_EVENT_DATA_BUFFER };
        if (xrPollEvent(m_instance, &event) != XR_SUCCESS)
        {
            return;
        }

        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            const auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            m_sessionState = changed.state;
            XVR_LOG("session state -> %d", static_cast<int>(m_sessionState));

            if (m_sessionState == XR_SESSION_STATE_READY)
            {
                XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
                beginInfo.primaryViewConfigurationType =
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (Check(xrBeginSession(m_session, &beginInfo), "xrBeginSession"))
                {
                    m_sessionRunning = true;
                }
            }
            else if (m_sessionState == XR_SESSION_STATE_STOPPING)
            {
                m_sessionRunning = false;
                xrEndSession(m_session);
            }
            else if (m_sessionState == XR_SESSION_STATE_EXITING ||
                     m_sessionState == XR_SESSION_STATE_LOSS_PENDING)
            {
                m_exitRequested = true;
            }
        }
        else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
        {
            m_exitRequested = true;
        }
    }
}

void XrApp::RenderFrame(XrTime predictedDisplayTime)
{
    XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = predictedDisplayTime;
    locateInfo.space = m_space;

    XrViewState viewState{ XR_TYPE_VIEW_STATE };
    uint32_t viewCount = 0;
    xrLocateViews(m_session, &locateInfo, &viewState, static_cast<uint32_t>(m_views.size()),
                  &viewCount, m_views.data());

    const float timeSeconds = static_cast<float>(predictedDisplayTime) * 1e-9f;

    // Send the viewpoint before rendering. The host needs it as early as possible: every
    // millisecond between locating the views here and the host rendering against them is
    // motion-to-photon latency the wearer feels directly.
    if (m_poseCallback != nullptr && viewCount >= 2)
    {
        // Head pose taken as the midpoint between the eyes, which is what the host uses
        // for anything not strictly per-eye.
        float head[7];
        for (int i = 0; i < 3; ++i)
        {
            const float* left = &m_views[0].pose.position.x;
            const float* right = &m_views[1].pose.position.x;
            head[i] = (left[i] + right[i]) * 0.5f;
        }
        head[3] = m_views[0].pose.orientation.x;
        head[4] = m_views[0].pose.orientation.y;
        head[5] = m_views[0].pose.orientation.z;
        head[6] = m_views[0].pose.orientation.w;

        float eyes[22];
        for (int eye = 0; eye < 2; ++eye)
        {
            float* out = eyes + eye * 11;
            out[0] = m_views[eye].pose.position.x;
            out[1] = m_views[eye].pose.position.y;
            out[2] = m_views[eye].pose.position.z;
            out[3] = m_views[eye].pose.orientation.x;
            out[4] = m_views[eye].pose.orientation.y;
            out[5] = m_views[eye].pose.orientation.z;
            out[6] = m_views[eye].pose.orientation.w;
            out[7] = m_views[eye].fov.angleLeft;
            out[8] = m_views[eye].fov.angleRight;
            out[9] = m_views[eye].fov.angleUp;
            out[10] = m_views[eye].fov.angleDown;
        }

        m_poseCallback(head, eyes, m_poseCallbackContext);
    }

    std::vector<XrCompositionLayerProjectionView> projectionViews(viewCount);

    for (uint32_t i = 0; i < viewCount; ++i)
    {
        SwapchainInfo& swapchain = m_swapchains[i];

        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        xrAcquireSwapchainImage(swapchain.handle, &acquireInfo, &imageIndex);

        XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        waitInfo.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(swapchain.handle, &waitInfo);

        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               swapchain.images[imageIndex].image, 0);

        float projection[16];
        float view[16];
        float viewProjection[16];
        ProjectionFromFov(m_views[i].fov, 0.05f, 100.0f, projection);
        ViewFromPose(m_views[i].pose, view);
        MultiplyMatrices(projection, view, viewProjection);

        m_renderer.DrawEye(static_cast<int>(i), swapchain.width, swapchain.height, m_status,
                           timeSeconds, viewProjection);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(swapchain.handle, &releaseInfo);

        projectionViews[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        projectionViews[i].pose = m_views[i].pose;
        projectionViews[i].fov = m_views[i].fov;
        projectionViews[i].subImage.swapchain = swapchain.handle;
        projectionViews[i].subImage.imageRect.offset = { 0, 0 };
        projectionViews[i].subImage.imageRect.extent = { swapchain.width, swapchain.height };
    }

    XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    layer.space = m_space;
    layer.viewCount = viewCount;
    layer.views = projectionViews.data();

    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)
    };

    XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = 1;
    endInfo.layers = layers;
    xrEndFrame(m_session, &endInfo);
}

bool XrApp::Tick()
{
    PollEvents();

    if (m_exitRequested)
    {
        return false;
    }
    if (!m_sessionRunning)
    {
        return true;
    }

    XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState frameState{ XR_TYPE_FRAME_STATE };
    if (!XR_SUCCEEDED(xrWaitFrame(m_session, &waitInfo, &frameState)))
    {
        return true;
    }

    XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
    xrBeginFrame(m_session, &beginInfo);

    // SurfaceTexture updates must happen on the thread holding the GL context, which is
    // this one. Doing it here also means the newest decoded frame is picked up as late as
    // possible before rendering.
    if (m_frameCallback != nullptr)
    {
        m_frameCallback(m_frameCallbackContext);
    }

    // Controller state is sampled for the same predicted display time as the views, so
    // hands and head describe the same instant rather than drifting apart.
    UpdateControllers(frameState.predictedDisplayTime);

    if (frameState.shouldRender != 0)
    {
        RenderFrame(frameState.predictedDisplayTime);
    }
    else
    {
        // Still must end the frame, or the runtime considers us unresponsive.
        XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        xrEndFrame(m_session, &endInfo);
    }

    return true;
}

void XrApp::Shutdown()
{
    m_renderer.Shutdown();

    for (SwapchainInfo& swapchain : m_swapchains)
    {
        if (swapchain.handle != XR_NULL_HANDLE)
        {
            xrDestroySwapchain(swapchain.handle);
        }
    }
    m_swapchains.clear();

    if (m_space != XR_NULL_HANDLE)
    {
        xrDestroySpace(m_space);
        m_space = XR_NULL_HANDLE;
    }
    if (m_session != XR_NULL_HANDLE)
    {
        xrDestroySession(m_session);
        m_session = XR_NULL_HANDLE;
    }
    if (m_instance != XR_NULL_HANDLE)
    {
        xrDestroyInstance(m_instance);
        m_instance = XR_NULL_HANDLE;
    }
}

} // namespace xvr
