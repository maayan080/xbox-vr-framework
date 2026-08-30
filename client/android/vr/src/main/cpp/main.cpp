#include "XvrVr.h"

#include <android_native_app_glue.h>
#include <jni.h>

#include <chrono>
#include <cmath>

namespace {

struct AppState
{
    xvr::XrApp xr;
    android_app* app = nullptr;

    JavaVM* vm = nullptr;
    jobject activity = nullptr;

    jmethodID attachTexturesMethod = nullptr;
    jmethodID updateTexturesMethod = nullptr;
    jmethodID statusMethod = nullptr;
    jmethodID submitPoseMethod = nullptr;
    jmethodID lookupPoseMethod = nullptr;
    jmethodID renderPoseSequenceMethod = nullptr;
    jmethodID submitControllersMethod = nullptr;

    bool texturesAttached = false;
    bool destroyRequested = false;

    std::chrono::steady_clock::time_point lastFrameTime;
    bool everReceivedFrame = false;
};

AppState g_state;

/** Attaches this thread to the VM so Java can be called from the render loop. */
JNIEnv* GetEnv()
{
    JNIEnv* env = nullptr;
    if (g_state.vm == nullptr)
    {
        return nullptr;
    }
    if (g_state.vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
    {
        if (g_state.vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
        {
            return nullptr;
        }
    }
    return env;
}

bool ResolveJavaMethods()
{
    JNIEnv* env = GetEnv();
    if (env == nullptr || g_state.activity == nullptr)
    {
        return false;
    }

    jclass activityClass = env->GetObjectClass(g_state.activity);
    g_state.attachTexturesMethod = env->GetMethodID(activityClass, "attachEyeTextures", "(II)V");
    g_state.updateTexturesMethod = env->GetMethodID(activityClass, "updateEyeTextures", "()Z");
    g_state.statusMethod = env->GetMethodID(activityClass, "getStreamStatus", "()I");
    g_state.submitPoseMethod = env->GetMethodID(activityClass, "submitPose", "([F[F)V");
    g_state.lookupPoseMethod = env->GetMethodID(activityClass, "lookupPose", "(I)[F");
    g_state.renderPoseSequenceMethod =
        env->GetMethodID(activityClass, "getRenderPoseSequence", "(I)I");
    g_state.submitControllersMethod =
        env->GetMethodID(activityClass, "submitControllers", "([F)V");
    env->DeleteLocalRef(activityClass);

    if (g_state.attachTexturesMethod == nullptr || g_state.updateTexturesMethod == nullptr ||
        g_state.statusMethod == nullptr)
    {
        XVR_ERR("could not resolve VrActivity methods");
        return false;
    }
    return true;
}

/**
 * Runs on the render thread with the GL context current, which is the only place a
 * SurfaceTexture may be updated. Pulling the newest decoded frame here also keeps it as
 * fresh as possible at the moment it is drawn.
 */
void OnFrame(void*)
{
    JNIEnv* env = GetEnv();
    if (env == nullptr)
    {
        return;
    }

    if (!g_state.texturesAttached)
    {
        env->CallVoidMethod(g_state.activity, g_state.attachTexturesMethod,
                            static_cast<jint>(g_state.xr.Renderer().EyeTexture(0)),
                            static_cast<jint>(g_state.xr.Renderer().EyeTexture(1)));
        g_state.texturesAttached = true;
        XVR_LOG("eye textures handed to Java");
    }

    const jboolean hasNewFrame =
        env->CallBooleanMethod(g_state.activity, g_state.updateTexturesMethod);

    const jint status = env->CallIntMethod(g_state.activity, g_state.statusMethod);

    // Streaming is latched with a timeout rather than requiring a new frame on this exact
    // tick. The headset renders at its own refresh rate, which rarely matches the video's,
    // so most frames legitimately have no new image - and treating that as "not streaming"
    // made the status flash even while the picture was perfectly fine. The last decoded
    // frame stays on screen; only a real stall should change the status.
    const auto now = std::chrono::steady_clock::now();
    if (hasNewFrame == JNI_TRUE)
    {
        g_state.lastFrameTime = now;
        g_state.everReceivedFrame = true;
    }

    auto mapped = static_cast<xvr::Status>(status);
    if (g_state.everReceivedFrame &&
        now - g_state.lastFrameTime < std::chrono::milliseconds(400))
    {
        mapped = xvr::Status::Streaming;
    }
    g_state.xr.SetStatus(mapped);
}

/** 3x3 rotation from a quaternion, column-major to match glUniformMatrix3fv. */
void RotationFromQuaternion(const float* q, float* out)
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float x2 = x + x, y2 = y + y, z2 = z + z;
    const float xx = x * x2, xy = x * y2, xz = x * z2;
    const float yy = y * y2, yz = y * z2, zz = z * z2;
    const float wx = w * x2, wy = w * y2, wz = w * z2;

    out[0] = 1.0f - (yy + zz); out[1] = xy + wz;          out[2] = xz - wy;
    out[3] = xy - wz;          out[4] = 1.0f - (xx + zz); out[5] = yz + wx;
    out[6] = xz + wy;          out[7] = yz - wx;          out[8] = 1.0f - (xx + yy);
}

void MultiplyTransposed(const float* a, const float* b, float* out)
{
    // out = transpose(a) * b, both column-major 3x3.
    for (int column = 0; column < 3; ++column)
    {
        for (int row = 0; row < 3; ++row)
        {
            float sum = 0.0f;
            for (int i = 0; i < 3; ++i)
            {
                sum += a[row * 3 + i] * b[column * 3 + i];
            }
            out[column * 3 + row] = sum;
        }
    }
}

/**
 * Works out how to re-aim the frame on screen at where the head is now.
 *
 * The frame was rendered for the pose whose sequence the host echoed back. Asking Java for
 * that exact pose - rather than assuming the newest one - is what makes the correction
 * accurate instead of approximate.
 */
void UpdateReprojection(JNIEnv* env, xvr::XrApp& app, const float* currentEyes)
{
    // Resolved per eye. The eyes are separate streams and their decoders latch
    // independently, so they are not always displaying the same moment - warping both
    // with one eye's pose leaves the other slightly and visibly wrong.
    for (int eye = 0; eye < 2; ++eye)
    {
        const float* now = currentEyes + eye * 11;

        float currentFovTan[4];
        for (int i = 0; i < 4; ++i)
        {
            currentFovTan[i] = std::tan(now[7 + i]);
        }

        jint renderSequence = 0;
        if (g_state.renderPoseSequenceMethod != nullptr)
        {
            renderSequence = env->CallIntMethod(g_state.activity,
                                                g_state.renderPoseSequenceMethod,
                                                static_cast<jint>(eye));
        }

        jfloatArray renderEyes = nullptr;
        if (renderSequence != 0 && g_state.lookupPoseMethod != nullptr)
        {
            renderEyes = static_cast<jfloatArray>(env->CallObjectMethod(
                g_state.activity, g_state.lookupPoseMethod, renderSequence));
        }

        xvr::GlRenderer::Warp warp;
        for (int i = 0; i < 4; ++i)
        {
            warp.currentFovTan[i] = currentFovTan[i];
            warp.renderFovTan[i] = currentFovTan[i];
        }

        if (renderEyes == nullptr)
        {
            // Identity warp: show the frame unchanged. Stale, but warping against an
            // unknown viewpoint would be actively wrong rather than merely late.
            warp.valid = false;
            app.Renderer().SetReprojection(eye, warp);
            continue;
        }

        float renderData[22]{};
        env->GetFloatArrayRegion(renderEyes, 0, 22, renderData);
        env->DeleteLocalRef(renderEyes);

        const float* rendered = renderData + eye * 11;

        for (int i = 0; i < 4; ++i)
        {
            warp.renderFovTan[i] = std::tan(rendered[7 + i]);
        }

        float rotationNow[9];
        float rotationRender[9];
        RotationFromQuaternion(now + 3, rotationNow);
        RotationFromQuaternion(rendered + 3, rotationRender);

        // Current eye space -> world -> the space the frame was rendered in.
        MultiplyTransposed(rotationRender, rotationNow, warp.reprojection);

        for (int i = 0; i < 9; ++i)
        {
            warp.currentRotation[i] = rotationNow[i];
        }
        // Transpose of the render rotation maps world into that frame's eye space.
        for (int column = 0; column < 3; ++column)
        {
            for (int row = 0; row < 3; ++row)
            {
                warp.renderRotationInv[column * 3 + row] = rotationRender[row * 3 + column];
            }
        }

        for (int i = 0; i < 3; ++i)
        {
            warp.currentPosition[i] = now[i];
            warp.renderPosition[i] = rendered[i];
        }

        warp.valid = true;
        app.Renderer().SetReprojection(eye, warp);
    }
}

/** Hands the viewpoint OpenXR just reported to Java, which puts it on the wire. */
void OnPose(const float* head, const float* eyes, void*)
{
    JNIEnv* env = GetEnv();
    if (env == nullptr || g_state.submitPoseMethod == nullptr)
    {
        return;
    }

    jfloatArray headArray = env->NewFloatArray(7);
    jfloatArray eyeArray = env->NewFloatArray(22);
    env->SetFloatArrayRegion(headArray, 0, 7, head);
    env->SetFloatArrayRegion(eyeArray, 0, 22, eyes);

    env->CallVoidMethod(g_state.activity, g_state.submitPoseMethod, headArray, eyeArray);

    // Local references accumulate across frames and would exhaust the table within
    // seconds at 120Hz.
    env->DeleteLocalRef(headArray);
    env->DeleteLocalRef(eyeArray);

    // Computed here, with the freshest pose, and applied when this frame is drawn moments
    // later. Any delay between the two is latency reprojection cannot recover.
    UpdateReprojection(env, g_state.xr, eyes);
}

/** Hands controller state to Java, which stores it for the next pose packet. */
void OnControllers(const float* state, void*)
{
    JNIEnv* env = GetEnv();
    if (env == nullptr || g_state.submitControllersMethod == nullptr)
    {
        return;
    }

    constexpr int kTotal = 2 * xvr::XrApp::kControllerFloats;
    jfloatArray array = env->NewFloatArray(kTotal);
    env->SetFloatArrayRegion(array, 0, kTotal, state);
    env->CallVoidMethod(g_state.activity, g_state.submitControllersMethod, array);
    env->DeleteLocalRef(array);
}

void HandleCommand(android_app* app, int32_t command)
{
    switch (command)
    {
    case APP_CMD_DESTROY:
        g_state.destroyRequested = true;
        break;
    default:
        break;
    }
}

} // namespace

extern "C" void android_main(android_app* app)
{
    g_state.app = app;
    g_state.vm = app->activity->vm;
    g_state.activity = app->activity->clazz;

    app->onAppCmd = HandleCommand;

    XVR_LOG("starting");

    if (!ResolveJavaMethods())
    {
        XVR_ERR("Java bridge unavailable; aborting");
        return;
    }

    if (!g_state.xr.Initialize(app))
    {
        XVR_ERR("OpenXR initialisation failed");
        return;
    }

    g_state.xr.SetFrameCallback(OnFrame, nullptr);
    g_state.xr.SetPoseCallback(OnPose, nullptr);
    g_state.xr.SetControllerCallback(OnControllers, nullptr);

    while (!app->destroyRequested && !g_state.destroyRequested)
    {
        // Drain Android events. Timeout is zero while the session runs so the frame loop
        // is paced by xrWaitFrame rather than by the event queue.
        int events = 0;
        android_poll_source* source = nullptr;
        const int timeout = g_state.xr.IsSessionRunning() ? 0 : 200;

        while (ALooper_pollOnce(timeout, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0)
        {
            if (source != nullptr)
            {
                source->process(app, source);
            }
            if (app->destroyRequested)
            {
                break;
            }
        }

        if (!g_state.xr.Tick())
        {
            break;
        }
    }

    XVR_LOG("shutting down");
    g_state.xr.Shutdown();
}
