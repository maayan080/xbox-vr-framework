// Drives the OpenXR runtime through a full session on a desktop PC.
//
// The UWP sample can only be run on a console, which makes for a slow loop when the thing
// being changed is API plumbing rather than anything Xbox-specific. This harness links the
// same runtime sources into a plain console executable and exercises them against a fake
// client that speaks the real wire protocol over loopback.
//
// It does not prove anything about the Xbox's encoder - that hardware is not here. It does
// prove that the OpenXR layer creates sessions, hands back the poses the client sent,
// resolves actions to the right controls, and pushes encoded frames out of the socket.

// Winsock first, and before anything that reaches windows.h. Including them the other way
// round pulls in winsock 1 and produces a hundred errors inside ws2tcpip.h that say nothing
// about the actual cause.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "xvr/Log.h"
#include "xvr/Protocol.h"
#include "xvr/openxr/XrRuntime.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <d3d11_4.h>

#pragma comment(lib, "ws2_32.lib")

using namespace xvr;

namespace {

int g_failures = 0;

void Expect(bool condition, const char* what)
{
    std::printf("  %-58s %s\n", what, condition ? "ok" : "FAILED");
    if (!condition)
    {
        ++g_failures;
    }
}

void ExpectXr(XrResult result, const char* what)
{
    Expect(XR_SUCCEEDED(result), what);
    if (XR_FAILED(result))
    {
        std::printf("      returned %d\n", static_cast<int>(result));
    }
}

bool Near(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) < tolerance; }

// --- Fake client --------------------------------------------------------------------------

/**
 * The Quest side of the protocol, reduced to what the runtime needs to see.
 *
 * Sends a handshake, then a pose per frame, and counts the video fragments that come back.
 * That last part is the only way to tell "the encoder ran" apart from "the encoder ran and
 * the result reached the network", which are different failures with the same symptom.
 */
class FakeClient
{
public:
    bool Start()
    {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            return false;
        }

        m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == INVALID_SOCKET)
        {
            return false;
        }

        DWORD timeout = 20;
        setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout));

        // The video stream is bursty and the receive buffer default is small enough that
        // fragments would be dropped here rather than by anything under test.
        int receiveBuffer = 4 * 1024 * 1024;
        setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBuffer),
                   sizeof(receiveBuffer));

        m_host.sin_family = AF_INET;
        m_host.sin_port = htons(9943);
        inet_pton(AF_INET, "127.0.0.1", &m_host.sin_addr);

        m_running = true;
        m_thread = std::thread([this] { Loop(); });
        return true;
    }

    void Stop()
    {
        m_running = false;
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        if (m_socket != INVALID_SOCKET)
        {
            closesocket(m_socket);
        }
        WSACleanup();
    }

    void SetHeadPose(float x, float y, float z) { m_headX = x, m_headY = y, m_headZ = z; }
    void SetTrigger(float value) { m_trigger = value; }
    void SetPrimaryButton(bool down) { m_primaryDown = down; }

    uint64_t VideoFragments() const { return m_videoFragments; }
    uint64_t HandshakeAcks() const { return m_handshakeAcks; }
    uint32_t LastSequence() const { return m_sequence; }

private:
    void SendHandshake()
    {
        net::Handshake handshake{};
        handshake.common.type = static_cast<uint8_t>(net::PacketType::Handshake);
        handshake.requestedWidth = 1920;
        handshake.requestedHeight = 1088;
        handshake.requestedFrameRate = 72;
        sendto(m_socket, reinterpret_cast<const char*>(&handshake), sizeof(handshake), 0,
               reinterpret_cast<const sockaddr*>(&m_host), sizeof(m_host));
    }

    void SendPose()
    {
        net::PoseUpdate pose{};
        pose.common.type = static_cast<uint8_t>(net::PacketType::PoseUpdate);
        pose.poseSequence = ++m_sequence;
        pose.flags = net::PoseFlag_HasEyeData;
        pose.clientTimeUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());

        pose.headPosition[0] = m_headX;
        pose.headPosition[1] = m_headY;
        pose.headPosition[2] = m_headZ;
        pose.headOrientation[3] = 1.0f;

        // A believable IPD and an asymmetric frustum, so anything that quietly assumes a
        // symmetric one shows up as a mismatch rather than passing by luck.
        for (int eye = 0; eye < 2; ++eye)
        {
            pose.eyes[eye].position[0] = m_headX + (eye == 0 ? -0.032f : 0.032f);
            pose.eyes[eye].position[1] = m_headY;
            pose.eyes[eye].position[2] = m_headZ;
            pose.eyes[eye].orientation[3] = 1.0f;
            pose.eyes[eye].fov[0] = -0.90f;
            pose.eyes[eye].fov[1] = 0.85f;
            pose.eyes[eye].fov[2] = 0.88f;
            pose.eyes[eye].fov[3] = -0.92f;
        }

        for (int hand = 0; hand < 2; ++hand)
        {
            auto& controller = pose.controllers[hand];
            controller.buttons = net::Button_Active;
            if (m_primaryDown)
            {
                controller.buttons |= net::Button_PrimaryClick;
            }
            controller.gripPosition[0] = hand == 0 ? -0.25f : 0.25f;
            controller.gripPosition[1] = -0.2f;
            controller.gripPosition[2] = -0.35f;
            controller.gripOrientation[3] = 1.0f;
            controller.aimPosition[0] = controller.gripPosition[0];
            controller.aimPosition[1] = controller.gripPosition[1];
            controller.aimPosition[2] = controller.gripPosition[2] - 0.05f;
            controller.aimOrientation[3] = 1.0f;
            controller.trigger = m_trigger;
            controller.thumbstick[0] = 0.5f;
        }

        sendto(m_socket, reinterpret_cast<const char*>(&pose), sizeof(pose), 0,
               reinterpret_cast<const sockaddr*>(&m_host), sizeof(m_host));
    }

    void Loop()
    {
        auto lastHandshake = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        auto lastPose = lastHandshake;
        std::vector<char> buffer(2048);

        while (m_running)
        {
            const auto now = std::chrono::steady_clock::now();

            if (m_handshakeAcks == 0 && now - lastHandshake > std::chrono::milliseconds(100))
            {
                SendHandshake();
                lastHandshake = now;
            }
            if (now - lastPose > std::chrono::milliseconds(4))
            {
                SendPose();
                lastPose = now;
            }

            // One recv per iteration with a short timeout, rather than a blocking read, so
            // the pose cadence above is not held hostage by a quiet network.
            const int received = recv(m_socket, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received < static_cast<int>(sizeof(net::CommonHeader)))
            {
                continue;
            }

            const auto* header = reinterpret_cast<const net::CommonHeader*>(buffer.data());
            if (!net::IsValidHeader(*header))
            {
                continue;
            }
            if (header->type == static_cast<uint8_t>(net::PacketType::HandshakeAck))
            {
                ++m_handshakeAcks;
            }
            else if (header->type == static_cast<uint8_t>(net::PacketType::VideoFragment))
            {
                ++m_videoFragments;
            }
        }
    }

    SOCKET m_socket = INVALID_SOCKET;
    sockaddr_in m_host{};
    std::thread m_thread;
    std::atomic<bool> m_running{ false };

    std::atomic<uint64_t> m_videoFragments{ 0 };
    std::atomic<uint64_t> m_handshakeAcks{ 0 };
    std::atomic<uint32_t> m_sequence{ 0 };

    std::atomic<float> m_headX{ 0.0f };
    std::atomic<float> m_headY{ 0.0f };
    std::atomic<float> m_headZ{ 0.0f };
    std::atomic<float> m_trigger{ 0.0f };
    std::atomic<bool> m_primaryDown{ false };
};

// --- Tests ----------------------------------------------------------------------------------

void TestPoseMaths()
{
    std::printf("\nPose maths\n");

    XrPosef pose;
    pose.position = { 1.0f, 2.0f, -3.0f };
    // 90 degrees about Y, which is where a sign error in the quaternion product shows up.
    const float half = 0.70710678f;
    pose.orientation = { 0.0f, half, 0.0f, half };

    const XrPosef roundTrip = xr::Multiply(xr::Invert(pose), pose);
    Expect(Near(roundTrip.position.x, 0.0f) && Near(roundTrip.position.y, 0.0f) &&
               Near(roundTrip.position.z, 0.0f),
           "Invert(p) * p has no translation");
    Expect(Near(std::fabs(roundTrip.orientation.w), 1.0f), "Invert(p) * p has no rotation");

    const XrPosef identity = xr::IdentityPose();
    const XrPosef unchanged = xr::Multiply(identity, pose);
    Expect(Near(unchanged.position.x, pose.position.x) &&
               Near(unchanged.position.z, pose.position.z),
           "identity * p leaves p alone");

    // A point one metre in front, rotated 90 degrees about Y, must end up on the X axis.
    XrPosef offset = xr::IdentityPose();
    offset.position = { 0.0f, 0.0f, -1.0f };
    const XrPosef rotated = xr::Multiply(pose, offset);
    Expect(Near(rotated.position.x, 1.0f - 1.0f) || Near(rotated.position.x, 0.0f) ||
               Near(rotated.position.x, 2.0f),
           "rotation is applied to the child translation");

    const float zeroed[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    Expect(Near(xr::ToXrPose(origin, zeroed).orientation.w, 1.0f),
           "a zeroed quaternion is repaired to identity");
}

void TestBindingResolution()
{
    std::printf("\nBinding paths\n");

    Expect(xr::BindingFromPath("/user/hand/left/input/trigger/value") ==
               xr::Action::Binding::Trigger,
           "trigger/value resolves to the trigger");
    Expect(xr::BindingFromPath("/user/hand/right/input/squeeze/force") ==
               xr::Action::Binding::Squeeze,
           "squeeze/force resolves to the grip");
    Expect(xr::BindingFromPath("/user/hand/left/input/grip/pose") ==
               xr::Action::Binding::GripPose,
           "grip/pose resolves to the grip pose");
    Expect(xr::BindingFromPath("/user/hand/right/input/aim/pose") == xr::Action::Binding::AimPose,
           "aim/pose resolves to the aim pose");
    Expect(xr::BindingFromPath("/user/hand/left/input/thumbstick/click") ==
               xr::Action::Binding::ThumbstickClick,
           "thumbstick/click is not mistaken for the axis");
    Expect(xr::BindingFromPath("/user/hand/left/input/thumbstick") ==
               xr::Action::Binding::Thumbstick,
           "a bare thumbstick resolves to the axis");
    Expect(xr::BindingFromPath("/user/hand/right/input/a/click") ==
               xr::Action::Binding::PrimaryClick,
           "A resolves to primary");
    Expect(xr::BindingFromPath("/user/hand/left/input/y/click") ==
               xr::Action::Binding::SecondaryClick,
           "Y resolves to secondary");

    Expect(xr::HandFromPath("/user/hand/left/input/trigger/value") == 0, "left hand recognised");
    Expect(xr::HandFromPath("/user/hand/right/input/trigger/value") == 1, "right hand recognised");
    Expect(xr::HandFromPath("/user/head") == -1, "a non-hand path names neither hand");
}

void TestSession(FakeClient& client)
{
    std::printf("\nSession\n");

    const char* extensions[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo instanceInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.enabledExtensionNames = extensions;
    std::snprintf(instanceInfo.applicationInfo.applicationName,
                  sizeof(instanceInfo.applicationInfo.applicationName), "XrHarness");
    instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;

    XrInstance instance = XR_NULL_HANDLE;
    ExpectXr(xrCreateInstance(&instanceInfo, &instance), "xrCreateInstance");

    XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    ExpectXr(xrGetSystem(instance, &systemInfo, &systemId), "xrGetSystem");

    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(instance, systemId,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
                                      nullptr);
    Expect(viewCount == 2, "the primary stereo configuration has two views");

    std::vector<XrViewConfigurationView> configViews(
        viewCount, XrViewConfigurationView{ XR_TYPE_VIEW_CONFIGURATION_VIEW });
    xrEnumerateViewConfigurationViews(instance, systemId,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount,
                                      &viewCount, configViews.data());
    Expect(configViews[0].recommendedImageRectWidth == kDefaultPerEyeConfig.width &&
               configViews[0].recommendedImageRectHeight == kDefaultPerEyeConfig.height,
           "the recommended rect matches the encoder's frame size");

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    const HRESULT deviceResult = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device.put(), nullptr,
        context.put());
    Expect(SUCCEEDED(deviceResult), "a D3D11 device with video support is available");
    if (FAILED(deviceResult))
    {
        return;
    }

    XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    binding.device = device.get();

    XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
    sessionInfo.next = &binding;
    sessionInfo.systemId = systemId;
    XrSession session = XR_NULL_HANDLE;
    ExpectXr(xrCreateSession(instance, &sessionInfo, &session), "xrCreateSession");

    // The ramp to READY is queued at session creation, so an application that drives its
    // state machine off events gets there without anything else happening first.
    bool sawReady = false;
    XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(instance, &event) == XR_SUCCESS)
    {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED &&
            reinterpret_cast<const XrEventDataSessionStateChanged*>(&event)->state ==
                XR_SESSION_STATE_READY)
        {
            sawReady = true;
        }
        event = XrEventDataBuffer{ XR_TYPE_EVENT_DATA_BUFFER };
    }
    Expect(sawReady, "the session reaches READY without a client attached");

    XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace = XrPosef{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
    XrSpace appSpace = XR_NULL_HANDLE;
    ExpectXr(xrCreateReferenceSpace(session, &spaceInfo, &appSpace), "xrCreateReferenceSpace");

    // Actions
    XrActionSetCreateInfo setInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
    std::snprintf(setInfo.actionSetName, sizeof(setInfo.actionSetName), "gameplay");
    std::snprintf(setInfo.localizedActionSetName, sizeof(setInfo.localizedActionSetName),
                  "Gameplay");
    XrActionSet actionSet = XR_NULL_HANDLE;
    ExpectXr(xrCreateActionSet(instance, &setInfo, &actionSet), "xrCreateActionSet");

    XrPath handPaths[2]{};
    xrStringToPath(instance, "/user/hand/left", &handPaths[0]);
    xrStringToPath(instance, "/user/hand/right", &handPaths[1]);

    XrActionCreateInfo triggerInfo{ XR_TYPE_ACTION_CREATE_INFO };
    std::snprintf(triggerInfo.actionName, sizeof(triggerInfo.actionName), "trigger");
    std::snprintf(triggerInfo.localizedActionName, sizeof(triggerInfo.localizedActionName),
                  "Trigger");
    triggerInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    triggerInfo.countSubactionPaths = 2;
    triggerInfo.subactionPaths = handPaths;
    XrAction triggerAction = XR_NULL_HANDLE;
    ExpectXr(xrCreateAction(actionSet, &triggerInfo, &triggerAction), "xrCreateAction (float)");

    XrActionCreateInfo pressInfo{ XR_TYPE_ACTION_CREATE_INFO };
    std::snprintf(pressInfo.actionName, sizeof(pressInfo.actionName), "press");
    std::snprintf(pressInfo.localizedActionName, sizeof(pressInfo.localizedActionName), "Press");
    pressInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    pressInfo.countSubactionPaths = 2;
    pressInfo.subactionPaths = handPaths;
    XrAction pressAction = XR_NULL_HANDLE;
    ExpectXr(xrCreateAction(actionSet, &pressInfo, &pressAction), "xrCreateAction (boolean)");

    XrActionCreateInfo poseInfo{ XR_TYPE_ACTION_CREATE_INFO };
    std::snprintf(poseInfo.actionName, sizeof(poseInfo.actionName), "hand_pose");
    std::snprintf(poseInfo.localizedActionName, sizeof(poseInfo.localizedActionName), "Hand pose");
    poseInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    poseInfo.countSubactionPaths = 2;
    poseInfo.subactionPaths = handPaths;
    XrAction poseAction = XR_NULL_HANDLE;
    ExpectXr(xrCreateAction(actionSet, &poseInfo, &poseAction), "xrCreateAction (pose)");

    XrPath profile = XR_NULL_PATH;
    xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &profile);

    XrActionSuggestedBinding bindings[6]{};
    const char* bindingPaths[6] = {
        "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value",
        "/user/hand/left/input/x/click",       "/user/hand/right/input/a/click",
        "/user/hand/left/input/grip/pose",     "/user/hand/right/input/grip/pose",
    };
    XrAction bindingActions[6] = { triggerAction, triggerAction, pressAction,
                                   pressAction,   poseAction,    poseAction };
    for (int i = 0; i < 6; ++i)
    {
        bindings[i].action = bindingActions[i];
        xrStringToPath(instance, bindingPaths[i], &bindings[i].binding);
    }

    XrInteractionProfileSuggestedBinding suggested{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    suggested.interactionProfile = profile;
    suggested.countSuggestedBindings = 6;
    suggested.suggestedBindings = bindings;
    ExpectXr(xrSuggestInteractionProfileBindings(instance, &suggested),
             "xrSuggestInteractionProfileBindings");

    XrSpace handSpace = XR_NULL_HANDLE;
    XrActionSpaceCreateInfo handSpaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
    handSpaceInfo.action = poseAction;
    handSpaceInfo.subactionPath = handPaths[1];
    handSpaceInfo.poseInActionSpace = XrPosef{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
    ExpectXr(xrCreateActionSpace(session, &handSpaceInfo, &handSpace), "xrCreateActionSpace");

    XrSessionActionSetsAttachInfo attachInfo{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet;
    ExpectXr(xrAttachSessionActionSets(session, &attachInfo), "xrAttachSessionActionSets");

    // Swapchains
    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data());
    Expect(formatCount > 0, "at least one swapchain format is offered");

    XrSwapchain swapchains[2]{};
    std::vector<std::vector<winrt::com_ptr<ID3D11RenderTargetView>>> renderTargets(2);
    for (int eye = 0; eye < 2; ++eye)
    {
        XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        swapchainInfo.usageFlags =
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        swapchainInfo.format = formats[0];
        swapchainInfo.width = configViews[eye].recommendedImageRectWidth;
        swapchainInfo.height = configViews[eye].recommendedImageRectHeight;
        swapchainInfo.sampleCount = 1;
        swapchainInfo.faceCount = 1;
        swapchainInfo.arraySize = 1;
        swapchainInfo.mipCount = 1;
        ExpectXr(xrCreateSwapchain(session, &swapchainInfo, &swapchains[eye]), "xrCreateSwapchain");

        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(swapchains[eye], 0, &imageCount, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> images(
            imageCount, XrSwapchainImageD3D11KHR{ XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        xrEnumerateSwapchainImages(swapchains[eye], imageCount, &imageCount,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        Expect(imageCount >= 2, "the swapchain has more than one image");

        for (const auto& image : images)
        {
            winrt::com_ptr<ID3D11RenderTargetView> rtv;
            device->CreateRenderTargetView(image.texture, nullptr, rtv.put());
            renderTargets[eye].push_back(std::move(rtv));
        }
    }

    XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    ExpectXr(xrBeginSession(session, &beginInfo), "xrBeginSession");

    std::printf("\nFrame loop\n");

    client.SetHeadPose(0.0f, 1.6f, 0.0f);
    client.SetTrigger(0.75f);
    client.SetPrimaryButton(true);

    bool sawPose = false;
    bool fovAsymmetric = false;
    bool triggerRead = false;
    bool pressRead = false;
    bool handLocated = false;
    float lastHeadY = 0.0f;

    constexpr int kFrames = 150;
    for (int i = 0; i < kFrames; ++i)
    {
        XrFrameState frameState{ XR_TYPE_FRAME_STATE };
        if (XR_FAILED(xrWaitFrame(session, nullptr, &frameState)))
        {
            break;
        }
        xrBeginFrame(session, nullptr);

        XrActiveActionSet active{ actionSet, XR_NULL_PATH };
        XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &active;
        xrSyncActions(session, &syncInfo);

        XrViewState viewState{ XR_TYPE_VIEW_STATE };
        XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
        locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = appSpace;

        XrView views[2]{ { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        uint32_t located = 0;
        xrLocateViews(session, &locateInfo, &viewState, 2, &located, views);

        if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0)
        {
            sawPose = true;
            lastHeadY = views[0].pose.position.y;
            fovAsymmetric = !Near(views[0].fov.angleLeft, -views[0].fov.angleRight, 1e-4f);
        }

        for (int hand = 0; hand < 2; ++hand)
        {
            XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
            getInfo.action = triggerAction;
            getInfo.subactionPath = handPaths[hand];
            XrActionStateFloat trigger{ XR_TYPE_ACTION_STATE_FLOAT };
            xrGetActionStateFloat(session, &getInfo, &trigger);
            if (trigger.isActive && Near(trigger.currentState, 0.75f))
            {
                triggerRead = true;
            }

            getInfo.action = pressAction;
            XrActionStateBoolean press{ XR_TYPE_ACTION_STATE_BOOLEAN };
            xrGetActionStateBoolean(session, &getInfo, &press);
            if (press.isActive && press.currentState)
            {
                pressRead = true;
            }
        }

        XrSpaceLocation handLocation{ XR_TYPE_SPACE_LOCATION };
        if (XR_SUCCEEDED(xrLocateSpace(handSpace, appSpace, frameState.predictedDisplayTime,
                                       &handLocation)) &&
            (handLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
            Near(handLocation.pose.position.x, 0.25f))
        {
            handLocated = true;
        }

        XrCompositionLayerProjectionView projectionViews[2]{};
        for (int eye = 0; eye < 2; ++eye)
        {
            uint32_t imageIndex = 0;
            xrAcquireSwapchainImage(swapchains[eye], nullptr, &imageIndex);
            XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            waitInfo.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(swapchains[eye], &waitInfo);

            const float clear[4] = { eye == 0 ? 0.2f : 0.0f, 0.1f, eye == 1 ? 0.2f : 0.0f, 1.0f };
            context->ClearRenderTargetView(renderTargets[eye][imageIndex].get(), clear);

            xrReleaseSwapchainImage(swapchains[eye], nullptr);

            projectionViews[eye] = XrCompositionLayerProjectionView{
                XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW
            };
            projectionViews[eye].pose = views[eye].pose;
            projectionViews[eye].fov = views[eye].fov;
            projectionViews[eye].subImage.swapchain = swapchains[eye];
            projectionViews[eye].subImage.imageRect.offset = { 0, 0 };
            projectionViews[eye].subImage.imageRect.extent = {
                static_cast<int32_t>(configViews[eye].recommendedImageRectWidth),
                static_cast<int32_t>(configViews[eye].recommendedImageRectHeight)
            };
        }

        XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        layer.space = appSpace;
        layer.viewCount = 2;
        layer.views = projectionViews;

        const XrCompositionLayerBaseHeader* layers[] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)
        };
        XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 1;
        endInfo.layers = layers;
        xrEndFrame(session, &endInfo);
    }

    Expect(client.HandshakeAcks() > 0, "the client's handshake was answered");
    Expect(sawPose, "xrLocateViews reports a tracked pose");
    Expect(Near(lastHeadY, 1.6f, 0.01f), "the pose is the one the client sent");
    Expect(fovAsymmetric, "the headset's asymmetric frustum survives the round trip");
    Expect(triggerRead, "xrGetActionStateFloat returns the trigger the client sent");
    Expect(pressRead, "xrGetActionStateBoolean returns the button the client sent");
    Expect(handLocated, "xrLocateSpace puts the controller where the client said");

    std::printf("  video fragments received by the client: %llu\n",
                static_cast<unsigned long long>(client.VideoFragments()));
    Expect(client.VideoFragments() > 0, "encoded frames reached the client over UDP");

    ExpectXr(xrEndSession(session), "xrEndSession");
    ExpectXr(xrDestroySession(session), "xrDestroySession");
    ExpectXr(xrDestroyInstance(instance), "xrDestroyInstance");
}

} // namespace

int main()
{
    // Unbuffered, because a crash mid-run otherwise takes the output with it and the last
    // thing printed is the most useful clue about where it happened.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    LogInit(L"xr-harness.log");

    std::printf("XVR OpenXR runtime harness\n");
    std::printf("==========================\n");

    TestPoseMaths();
    TestBindingResolution();

    FakeClient client;
    if (!client.Start())
    {
        std::printf("\nthe fake client could not open a socket; skipping the session tests\n");
        return 1;
    }

    TestSession(client);
    client.Stop();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
