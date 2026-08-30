// A plain OpenXR application.
//
// Nothing in this file includes an XVR header or knows that the display is a Quest on the
// far end of a Wi-Fi link. It is written exactly as it would be for a desktop headset -
// instance, system, session, swapchains, frame loop, actions - and the only thing that
// makes it stream is which library it links against.
//
// That is the whole point of it. If this runs, an existing OpenXR application can be
// rebuilt against the runtime and stream without source changes.

#include <d3d11_4.h>
#include <DirectXMath.h>

#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>

#include "Shaders/Generated/CubePS.h"
#include "Shaders/Generated/CubeVS.h"

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;
using namespace DirectX;

namespace {

void Check(XrResult result, const char* what)
{
    if (XR_FAILED(result))
    {
        throw std::runtime_error(std::string(what) + " failed: " + std::to_string(result));
    }
}

void Check(HRESULT result, const char* what)
{
    if (FAILED(result))
    {
        throw std::runtime_error(std::string(what) + " failed");
    }
}

/** Fills one of OpenXR's fixed-size name fields. */
template <size_t N>
void SetName(char (&destination)[N], const char* text)
{
    std::snprintf(destination, N, "%s", text);
}

struct Vertex
{
    XMFLOAT3 position;
    XMFLOAT3 normal;
};

struct PerObject
{
    XMFLOAT4X4 modelViewProjection;
    XMFLOAT4 tint;
};

/**
 * Projection matrix from OpenXR's four frustum half-angles.
 *
 * The angles are asymmetric on real headsets and the two eyes do not match each other, so
 * this cannot be replaced with a field-of-view and an aspect ratio without putting the
 * image visibly in the wrong place.
 */
XMMATRIX ProjectionFromFov(const XrFovf& fov, float nearZ, float farZ)
{
    const float left = std::tan(fov.angleLeft);
    const float right = std::tan(fov.angleRight);
    const float down = std::tan(fov.angleDown);
    const float up = std::tan(fov.angleUp);

    const float width = right - left;
    const float height = up - down;

    // Reversed-Z is not used here; the depth buffer is throwaway.
    XMFLOAT4X4 m{};
    m._11 = 2.0f / width;
    m._31 = (right + left) / width;
    m._22 = 2.0f / height;
    m._32 = (up + down) / height;
    m._33 = -(farZ + nearZ) / (farZ - nearZ);
    m._34 = -1.0f;
    m._43 = -(farZ * (nearZ + nearZ)) / (farZ - nearZ);
    return XMLoadFloat4x4(&m);
}

XMMATRIX ViewFromPose(const XrPosef& pose)
{
    const XMVECTOR orientation =
        XMVectorSet(pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w);
    const XMVECTOR position = XMVectorSet(pose.position.x, pose.position.y, pose.position.z, 1.0f);
    const XMMATRIX toWorld = XMMatrixAffineTransformation(g_XMOne, g_XMZero, orientation, position);
    return XMMatrixInverse(nullptr, toWorld);
}

std::vector<Vertex> BuildCube()
{
    // Six faces, each with its own normal so the lighting has something to work with.
    const XMFLOAT3 normals[6] = { { 0, 0, 1 },  { 0, 0, -1 }, { 1, 0, 0 },
                                 { -1, 0, 0 }, { 0, 1, 0 },  { 0, -1, 0 } };
    const XMFLOAT3 faces[6][4] = {
        { { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } },
        { { 1, -1, -1 }, { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 } },
        { { 1, -1, 1 }, { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 } },
        { { -1, -1, -1 }, { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 } },
        { { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 }, { -1, 1, -1 } },
        { { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 } },
    };

    std::vector<Vertex> vertices;
    for (int face = 0; face < 6; ++face)
    {
        const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int i : order)
        {
            vertices.push_back(Vertex{ faces[face][i], normals[face] });
        }
    }
    return vertices;
}

struct Swapchain
{
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<com_ptr<ID3D11RenderTargetView>> renderTargets;
    com_ptr<ID3D11DepthStencilView> depthStencil;
    uint32_t width = 0;
    uint32_t height = 0;
};

class CubeApp
{
public:
    void Run()
    {
        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();

        CreateInstanceAndSystem();
        CreateDeviceAndSession();
        CreateSpacesAndSwapchains();
        CreateActions();
        CreateGeometry();

        while (!m_exiting)
        {
            window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            PumpEvents();

            if (!m_sessionRunning)
            {
                continue;
            }
            RenderFrame();
        }

        Teardown();
    }

private:
    void CreateInstanceAndSystem()
    {
        const char* extensions[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };

        XrInstanceCreateInfo instanceInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
        instanceInfo.enabledExtensionCount = 1;
        instanceInfo.enabledExtensionNames = extensions;
        SetName(instanceInfo.applicationInfo.applicationName, "OpenXrCube");
        instanceInfo.applicationInfo.applicationVersion = 1;
        SetName(instanceInfo.applicationInfo.engineName, "none");
        instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        Check(xrCreateInstance(&instanceInfo, &m_instance), "xrCreateInstance");

        XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        Check(xrGetSystem(m_instance, &systemInfo, &m_systemId), "xrGetSystem");
    }

    void CreateDeviceAndSession()
    {
        PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
        Check(xrGetInstanceProcAddr(m_instance, "xrGetD3D11GraphicsRequirementsKHR",
                                    reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)),
              "xrGetInstanceProcAddr");

        XrGraphicsRequirementsD3D11KHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
        Check(getRequirements(m_instance, m_systemId, &requirements),
              "xrGetD3D11GraphicsRequirementsKHR");

        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                m_device.put(), nullptr, m_context.put()),
              "D3D11CreateDevice");

        XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
        binding.device = m_device.get();

        XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
        sessionInfo.next = &binding;
        sessionInfo.systemId = m_systemId;
        Check(xrCreateSession(m_instance, &sessionInfo, &m_session), "xrCreateSession");
    }

    void CreateSpacesAndSwapchains()
    {
        XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        spaceInfo.poseInReferenceSpace = XrPosef{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
        Check(xrCreateReferenceSpace(m_session, &spaceInfo, &m_appSpace), "xrCreateReferenceSpace");

        uint32_t viewCount = 0;
        Check(xrEnumerateViewConfigurationViews(m_instance, m_systemId,
                                                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                                &viewCount, nullptr),
              "xrEnumerateViewConfigurationViews");

        std::vector<XrViewConfigurationView> configViews(
            viewCount, XrViewConfigurationView{ XR_TYPE_VIEW_CONFIGURATION_VIEW });
        Check(xrEnumerateViewConfigurationViews(m_instance, m_systemId,
                                                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                viewCount, &viewCount, configViews.data()),
              "xrEnumerateViewConfigurationViews");

        uint32_t formatCount = 0;
        Check(xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr),
              "xrEnumerateSwapchainFormats");
        std::vector<int64_t> formats(formatCount);
        Check(xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data()),
              "xrEnumerateSwapchainFormats");

        const int64_t colourFormat = formats.empty() ? DXGI_FORMAT_B8G8R8A8_UNORM : formats[0];

        m_views.resize(viewCount, XrView{ XR_TYPE_VIEW });
        m_swapchains.resize(viewCount);

        for (uint32_t eye = 0; eye < viewCount; ++eye)
        {
            XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            swapchainInfo.usageFlags =
                XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            swapchainInfo.format = colourFormat;
            swapchainInfo.width = configViews[eye].recommendedImageRectWidth;
            swapchainInfo.height = configViews[eye].recommendedImageRectHeight;
            swapchainInfo.sampleCount = 1;
            swapchainInfo.faceCount = 1;
            swapchainInfo.arraySize = 1;
            swapchainInfo.mipCount = 1;

            Swapchain& swapchain = m_swapchains[eye];
            swapchain.width = swapchainInfo.width;
            swapchain.height = swapchainInfo.height;
            Check(xrCreateSwapchain(m_session, &swapchainInfo, &swapchain.handle),
                  "xrCreateSwapchain");

            uint32_t imageCount = 0;
            Check(xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr),
                  "xrEnumerateSwapchainImages");
            std::vector<XrSwapchainImageD3D11KHR> images(
                imageCount, XrSwapchainImageD3D11KHR{ XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
            Check(xrEnumerateSwapchainImages(
                      swapchain.handle, imageCount, &imageCount,
                      reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
                  "xrEnumerateSwapchainImages");

            for (const auto& image : images)
            {
                com_ptr<ID3D11RenderTargetView> rtv;
                D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
                rtvDesc.Format = static_cast<DXGI_FORMAT>(colourFormat);
                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                Check(m_device->CreateRenderTargetView(image.texture, &rtvDesc, rtv.put()),
                      "CreateRenderTargetView");
                swapchain.renderTargets.push_back(std::move(rtv));
            }

            // The depth buffer is the application's own business; OpenXR does not require it
            // to be a swapchain unless the depth layer extension is in use.
            D3D11_TEXTURE2D_DESC depthDesc{};
            depthDesc.Width = swapchain.width;
            depthDesc.Height = swapchain.height;
            depthDesc.MipLevels = 1;
            depthDesc.ArraySize = 1;
            depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
            depthDesc.SampleDesc.Count = 1;
            depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

            com_ptr<ID3D11Texture2D> depthTexture;
            Check(m_device->CreateTexture2D(&depthDesc, nullptr, depthTexture.put()),
                  "CreateTexture2D");
            Check(m_device->CreateDepthStencilView(depthTexture.get(), nullptr,
                                                   swapchain.depthStencil.put()),
                  "CreateDepthStencilView");
        }
    }

    void CreateActions()
    {
        XrActionSetCreateInfo setInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
        SetName(setInfo.actionSetName, "gameplay");
        SetName(setInfo.localizedActionSetName, "Gameplay");
        Check(xrCreateActionSet(m_instance, &setInfo, &m_actionSet), "xrCreateActionSet");

        Check(xrStringToPath(m_instance, "/user/hand/left", &m_handPaths[0]), "xrStringToPath");
        Check(xrStringToPath(m_instance, "/user/hand/right", &m_handPaths[1]), "xrStringToPath");

        XrActionCreateInfo poseInfo{ XR_TYPE_ACTION_CREATE_INFO };
        SetName(poseInfo.actionName, "hand_pose");
        SetName(poseInfo.localizedActionName, "Hand pose");
        poseInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
        poseInfo.countSubactionPaths = 2;
        poseInfo.subactionPaths = m_handPaths.data();
        Check(xrCreateAction(m_actionSet, &poseInfo, &m_poseAction), "xrCreateAction");

        XrActionCreateInfo triggerInfo{ XR_TYPE_ACTION_CREATE_INFO };
        SetName(triggerInfo.actionName, "trigger");
        SetName(triggerInfo.localizedActionName, "Trigger");
        triggerInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
        triggerInfo.countSubactionPaths = 2;
        triggerInfo.subactionPaths = m_handPaths.data();
        Check(xrCreateAction(m_actionSet, &triggerInfo, &m_triggerAction), "xrCreateAction");

        XrPath profile = XR_NULL_PATH;
        Check(xrStringToPath(m_instance, "/interaction_profiles/oculus/touch_controller", &profile),
              "xrStringToPath");

        std::array<XrActionSuggestedBinding, 4> bindings{};
        const char* paths[4] = {
            "/user/hand/left/input/grip/pose",
            "/user/hand/right/input/grip/pose",
            "/user/hand/left/input/trigger/value",
            "/user/hand/right/input/trigger/value",
        };
        for (size_t i = 0; i < bindings.size(); ++i)
        {
            bindings[i].action = i < 2 ? m_poseAction : m_triggerAction;
            Check(xrStringToPath(m_instance, paths[i], &bindings[i].binding), "xrStringToPath");
        }

        XrInteractionProfileSuggestedBinding suggested{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING
        };
        suggested.interactionProfile = profile;
        suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        Check(xrSuggestInteractionProfileBindings(m_instance, &suggested),
              "xrSuggestInteractionProfileBindings");

        for (int hand = 0; hand < 2; ++hand)
        {
            XrActionSpaceCreateInfo spaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
            spaceInfo.action = m_poseAction;
            spaceInfo.subactionPath = m_handPaths[hand];
            spaceInfo.poseInActionSpace = XrPosef{ { 0, 0, 0, 1 }, { 0, 0, 0 } };
            Check(xrCreateActionSpace(m_session, &spaceInfo, &m_handSpaces[hand]),
                  "xrCreateActionSpace");
        }

        XrSessionActionSetsAttachInfo attachInfo{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
        attachInfo.countActionSets = 1;
        attachInfo.actionSets = &m_actionSet;
        Check(xrAttachSessionActionSets(m_session, &attachInfo), "xrAttachSessionActionSets");
    }

    void CreateGeometry()
    {
        const std::vector<Vertex> vertices = BuildCube();
        m_vertexCount = static_cast<uint32_t>(vertices.size());

        D3D11_BUFFER_DESC vertexDesc{};
        vertexDesc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
        vertexDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vertexData{ vertices.data(), 0, 0 };
        Check(m_device->CreateBuffer(&vertexDesc, &vertexData, m_vertexBuffer.put()),
              "CreateBuffer");

        D3D11_BUFFER_DESC constantDesc{};
        constantDesc.ByteWidth = sizeof(PerObject);
        constantDesc.Usage = D3D11_USAGE_DYNAMIC;
        constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(m_device->CreateBuffer(&constantDesc, nullptr, m_constantBuffer.put()),
              "CreateBuffer");

        Check(m_device->CreateVertexShader(g_CubeVS, sizeof(g_CubeVS), nullptr,
                                           m_vertexShader.put()),
              "CreateVertexShader");
        Check(m_device->CreatePixelShader(g_CubePS, sizeof(g_CubePS), nullptr, m_pixelShader.put()),
              "CreatePixelShader");

        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        Check(m_device->CreateInputLayout(layout, 2, g_CubeVS, sizeof(g_CubeVS),
                                          m_inputLayout.put()),
              "CreateInputLayout");

        D3D11_DEPTH_STENCIL_DESC depthDesc{};
        depthDesc.DepthEnable = TRUE;
        depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
        Check(m_device->CreateDepthStencilState(&depthDesc, m_depthState.put()),
              "CreateDepthStencilState");
    }

    void PumpEvents()
    {
        XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(m_instance, &event) == XR_SUCCESS)
        {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                const auto& changed =
                    *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                m_sessionState = changed.state;

                if (m_sessionState == XR_SESSION_STATE_READY && !m_sessionRunning)
                {
                    XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
                    beginInfo.primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    Check(xrBeginSession(m_session, &beginInfo), "xrBeginSession");
                    m_sessionRunning = true;
                }
                else if (m_sessionState == XR_SESSION_STATE_STOPPING)
                {
                    xrEndSession(m_session);
                    m_sessionRunning = false;
                }
                else if (m_sessionState == XR_SESSION_STATE_EXITING)
                {
                    m_exiting = true;
                }
            }
            event = XrEventDataBuffer{ XR_TYPE_EVENT_DATA_BUFFER };
        }
    }

    void SyncInput()
    {
        XrActiveActionSet active{ m_actionSet, XR_NULL_PATH };
        XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &active;
        xrSyncActions(m_session, &syncInfo);

        for (int hand = 0; hand < 2; ++hand)
        {
            XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
            getInfo.action = m_triggerAction;
            getInfo.subactionPath = m_handPaths[hand];

            XrActionStateFloat trigger{ XR_TYPE_ACTION_STATE_FLOAT };
            xrGetActionStateFloat(m_session, &getInfo, &trigger);
            m_trigger[hand] = trigger.isActive ? trigger.currentState : 0.0f;

            XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
            if (XR_SUCCEEDED(xrLocateSpace(m_handSpaces[hand], m_appSpace, m_displayTime,
                                           &location)) &&
                (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0)
            {
                m_handPose[hand] = location.pose;
                m_handTracked[hand] = true;
            }
            else
            {
                m_handTracked[hand] = false;
            }
        }
    }

    void RenderFrame()
    {
        XrFrameState frameState{ XR_TYPE_FRAME_STATE };
        Check(xrWaitFrame(m_session, nullptr, &frameState), "xrWaitFrame");
        m_displayTime = frameState.predictedDisplayTime;

        Check(xrBeginFrame(m_session, nullptr), "xrBeginFrame");

        SyncInput();

        std::vector<XrCompositionLayerProjectionView> projectionViews;
        XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };

        if (frameState.shouldRender)
        {
            XrViewState viewState{ XR_TYPE_VIEW_STATE };
            XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
            locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locateInfo.displayTime = frameState.predictedDisplayTime;
            locateInfo.space = m_appSpace;

            uint32_t viewCount = 0;
            Check(xrLocateViews(m_session, &locateInfo, &viewState,
                                static_cast<uint32_t>(m_views.size()), &viewCount, m_views.data()),
                  "xrLocateViews");

            projectionViews.resize(viewCount);
            for (uint32_t eye = 0; eye < viewCount; ++eye)
            {
                Swapchain& swapchain = m_swapchains[eye];

                uint32_t imageIndex = 0;
                Check(xrAcquireSwapchainImage(swapchain.handle, nullptr, &imageIndex),
                      "xrAcquireSwapchainImage");

                XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                waitInfo.timeout = XR_INFINITE_DURATION;
                Check(xrWaitSwapchainImage(swapchain.handle, &waitInfo), "xrWaitSwapchainImage");

                RenderEye(swapchain, imageIndex, m_views[eye]);

                Check(xrReleaseSwapchainImage(swapchain.handle, nullptr),
                      "xrReleaseSwapchainImage");

                projectionViews[eye] = XrCompositionLayerProjectionView{
                    XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW
                };
                projectionViews[eye].pose = m_views[eye].pose;
                projectionViews[eye].fov = m_views[eye].fov;
                projectionViews[eye].subImage.swapchain = swapchain.handle;
                projectionViews[eye].subImage.imageRect.offset = { 0, 0 };
                projectionViews[eye].subImage.imageRect.extent = {
                    static_cast<int32_t>(swapchain.width), static_cast<int32_t>(swapchain.height)
                };
                projectionViews[eye].subImage.imageArrayIndex = 0;
            }

            layer.space = m_appSpace;
            layer.viewCount = static_cast<uint32_t>(projectionViews.size());
            layer.views = projectionViews.data();
        }

        const XrCompositionLayerBaseHeader* layers[] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)
        };

        XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = projectionViews.empty() ? 0 : 1;
        endInfo.layers = projectionViews.empty() ? nullptr : layers;
        Check(xrEndFrame(m_session, &endInfo), "xrEndFrame");

        m_frameIndex++;
    }

    void RenderEye(Swapchain& swapchain, uint32_t imageIndex, const XrView& view)
    {
        ID3D11RenderTargetView* rtv = swapchain.renderTargets[imageIndex].get();
        const float clear[4] = { 0.05f, 0.06f, 0.10f, 1.0f };
        m_context->ClearRenderTargetView(rtv, clear);
        m_context->ClearDepthStencilView(swapchain.depthStencil.get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        m_context->OMSetRenderTargets(1, &rtv, swapchain.depthStencil.get());
        m_context->OMSetDepthStencilState(m_depthState.get(), 0);

        D3D11_VIEWPORT viewport{ 0.0f,
                                 0.0f,
                                 static_cast<float>(swapchain.width),
                                 static_cast<float>(swapchain.height),
                                 0.0f,
                                 1.0f };
        m_context->RSSetViewports(1, &viewport);

        const UINT stride = sizeof(Vertex);
        const UINT offset = 0;
        ID3D11Buffer* vertexBuffer = m_vertexBuffer.get();
        m_context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        m_context->IASetInputLayout(m_inputLayout.get());
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_vertexShader.get(), nullptr, 0);
        m_context->PSSetShader(m_pixelShader.get(), nullptr, 0);

        const XMMATRIX viewMatrix = ViewFromPose(view.pose);
        const XMMATRIX projection = ProjectionFromFov(view.fov, 0.05f, 100.0f);
        const XMMATRIX viewProjection = viewMatrix * projection;

        const float time = static_cast<float>(m_frameIndex) * 0.016f;

        // A ring of cubes to look around at, and a floor line to give the horizon somewhere
        // to sit. Head tracking is obvious the moment the ring stays put while you turn.
        for (int i = 0; i < 8; ++i)
        {
            const float angle = static_cast<float>(i) * XM_2PI / 8.0f;
            const XMMATRIX model = XMMatrixScaling(0.2f, 0.2f, 0.2f) *
                                   XMMatrixRotationY(time + angle) *
                                   XMMatrixTranslation(std::cos(angle) * 2.0f, 0.0f,
                                                       std::sin(angle) * 2.0f);
            const XMFLOAT4 tint{ 0.3f + 0.7f * (i % 3 == 0), 0.3f + 0.7f * (i % 3 == 1),
                                 0.3f + 0.7f * (i % 3 == 2), 1.0f };
            DrawCube(model * viewProjection, tint);
        }

        for (int hand = 0; hand < 2; ++hand)
        {
            if (!m_handTracked[hand])
            {
                continue;
            }
            const XMVECTOR orientation =
                XMVectorSet(m_handPose[hand].orientation.x, m_handPose[hand].orientation.y,
                            m_handPose[hand].orientation.z, m_handPose[hand].orientation.w);
            const XMVECTOR position = XMVectorSet(m_handPose[hand].position.x,
                                                  m_handPose[hand].position.y,
                                                  m_handPose[hand].position.z, 1.0f);
            const XMMATRIX model =
                XMMatrixScaling(0.04f, 0.04f, 0.06f) *
                XMMatrixAffineTransformation(g_XMOne, g_XMZero, orientation, position);

            // Brightens with the trigger, which is the cheapest possible proof that the
            // action system is wired through to the real controller.
            const float pull = m_trigger[hand];
            DrawCube(model * viewProjection, XMFLOAT4{ 0.5f + 0.5f * pull, 0.5f, 0.5f, 1.0f });
        }
    }

    void DrawCube(const XMMATRIX& modelViewProjection, const XMFLOAT4& tint)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_context->Map(m_constantBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            return;
        }
        auto* constants = static_cast<PerObject*>(mapped.pData);
        XMStoreFloat4x4(&constants->modelViewProjection, XMMatrixTranspose(modelViewProjection));
        constants->tint = tint;
        m_context->Unmap(m_constantBuffer.get(), 0);

        ID3D11Buffer* buffer = m_constantBuffer.get();
        m_context->VSSetConstantBuffers(0, 1, &buffer);
        m_context->Draw(m_vertexCount, 0);
    }

    void Teardown()
    {
        for (auto& swapchain : m_swapchains)
        {
            if (swapchain.handle != XR_NULL_HANDLE)
            {
                xrDestroySwapchain(swapchain.handle);
            }
        }
        if (m_session != XR_NULL_HANDLE)
        {
            xrDestroySession(m_session);
        }
        if (m_instance != XR_NULL_HANDLE)
        {
            xrDestroyInstance(m_instance);
        }
    }

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_appSpace = XR_NULL_HANDLE;
    XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;
    XrTime m_displayTime = 0;
    bool m_sessionRunning = false;
    bool m_exiting = false;

    std::vector<XrView> m_views;
    std::vector<Swapchain> m_swapchains;

    XrActionSet m_actionSet = XR_NULL_HANDLE;
    XrAction m_poseAction = XR_NULL_HANDLE;
    XrAction m_triggerAction = XR_NULL_HANDLE;
    std::array<XrPath, 2> m_handPaths{};
    std::array<XrSpace, 2> m_handSpaces{};
    std::array<XrPosef, 2> m_handPose{};
    std::array<bool, 2> m_handTracked{};
    std::array<float, 2> m_trigger{};

    com_ptr<ID3D11Device> m_device;
    com_ptr<ID3D11DeviceContext> m_context;
    com_ptr<ID3D11Buffer> m_vertexBuffer;
    com_ptr<ID3D11Buffer> m_constantBuffer;
    com_ptr<ID3D11VertexShader> m_vertexShader;
    com_ptr<ID3D11PixelShader> m_pixelShader;
    com_ptr<ID3D11InputLayout> m_inputLayout;
    com_ptr<ID3D11DepthStencilState> m_depthState;
    uint32_t m_vertexCount = 0;
    uint64_t m_frameIndex = 0;
};

struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
    IFrameworkView CreateView() { return *this; }
    void Initialize(const CoreApplicationView&) {}
    void SetWindow(const CoreWindow&) {}
    void Load(const hstring&) {}
    void Uninitialize() {}
    void SetLogicalDpi(float) {}

    void Run()
    {
        CubeApp app;
        app.Run();
    }
};

} // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    init_apartment();
    CoreApplication::Run(make<App>());
    return 0;
}
