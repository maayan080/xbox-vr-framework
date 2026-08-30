#include "SceneRenderer.h"

#include "xvr/Check.h"
#include "xvr/Protocol.h"

#include "Shaders/Generated/ScenePS.h"
#include "Shaders/Generated/SceneVS.h"

#include <cmath>
#include <cstring>

namespace sample {
namespace {

constexpr uint32_t kCubeVertices = 36;
constexpr uint32_t kObjectCount = 6;
constexpr uint32_t kGridSize = 20;
constexpr uint32_t kFloorVertices = kGridSize * kGridSize * 6;
// Per hand: a cube plus six indicator squares.
constexpr uint32_t kHandVertices = (kCubeVertices + 6 * 6) * 2;
constexpr uint32_t kTotalVertices =
    kCubeVertices * kObjectCount + kFloorVertices + kHandVertices;

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
 * Asymmetric projection from the headset's reported field of view.
 *
 * A headset's per-eye frustum is not centred, so a symmetric perspective matrix would
 * misplace the whole scene and break fusion between the eyes.
 */
void ProjectionFromFov(const float fov[4], float nearZ, float farZ, float* out)
{
    const float tanLeft = std::tan(fov[0]);
    const float tanRight = std::tan(fov[1]);
    const float tanUp = std::tan(fov[2]);
    const float tanDown = std::tan(fov[3]);

    const float tanWidth = tanRight - tanLeft;
    const float tanHeight = tanUp - tanDown;

    std::memset(out, 0, sizeof(float) * 16);
    out[0] = 2.0f / tanWidth;
    out[5] = 2.0f / tanHeight;
    out[8] = (tanRight + tanLeft) / tanWidth;
    out[9] = (tanUp + tanDown) / tanHeight;
    out[10] = -(farZ + nearZ) / (farZ - nearZ);
    out[11] = -1.0f;
    out[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

/** View matrix from an eye pose: the inverse of its rotation and translation. */
void ViewFromPose(const float position[3], const float orientation[4], float* out)
{
    const float qx = orientation[0], qy = orientation[1], qz = orientation[2],
                qw = orientation[3];

    const float x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
    const float xx = qx * x2, xy = qx * y2, xz = qx * z2;
    const float yy = qy * y2, yz = qy * z2, zz = qz * z2;
    const float wx = qw * x2, wy = qw * y2, wz = qw * z2;

    // Transposed rotation, which for an orthonormal rotation is its inverse.
    const float rotation[16] = {
        1.0f - (yy + zz), xy - wz,          xz + wy,          0.0f,
        xy + wz,          1.0f - (xx + zz), yz - wx,          0.0f,
        xz - wy,          yz + wx,          1.0f - (xx + yy), 0.0f,
        0.0f,             0.0f,             0.0f,             1.0f
    };

    const float translation[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        -position[0], -position[1], -position[2], 1.0f
    };

    MultiplyMatrices(rotation, translation, out);
}

} // namespace

void SceneRenderer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_context = context;

    XVR_CHECK(device->CreateVertexShader(g_SceneVS, sizeof(g_SceneVS), nullptr,
                                         m_vertexShader.put()));
    XVR_CHECK(device->CreatePixelShader(g_ScenePS, sizeof(g_ScenePS), nullptr,
                                        m_pixelShader.put()));

    D3D11_BUFFER_DESC constantsDesc{};
    constantsDesc.ByteWidth = sizeof(SceneParams);
    constantsDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    XVR_CHECK(device->CreateBuffer(&constantsDesc, nullptr, m_constants.put()));

    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    // No culling: the cube winding is not carefully authored, and a missing face is a
    // worse failure than an unnecessary one being drawn.
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    XVR_CHECK(device->CreateRasterizerState(&rasterDesc, m_rasterizer.put()));

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
    XVR_CHECK(device->CreateDepthStencilState(&depthDesc, m_depthState.put()));
}

void SceneRenderer::RenderEye(const xvr::FrameContext& frame, xvr::Eye eye,
                              const xvr::EyeView& view)
{
    const auto eyeIndex = static_cast<int>(eye);

    SceneParams params{};

    if (frame.poseValid)
    {
        const xvr::EyeViewpoint& viewpoint = frame.eyes[eyeIndex];

        float projection[16];
        float viewMatrix[16];
        ProjectionFromFov(viewpoint.fov, 0.05f, 100.0f, projection);
        ViewFromPose(viewpoint.position, viewpoint.orientation, viewMatrix);
        MultiplyMatrices(projection, viewMatrix, params.viewProjection);
    }
    else
    {
        // No pose yet. Render from a fixed viewpoint with a plausible frustum rather than
        // nothing: the host streams before anyone is looking, and a black frame would make
        // a pose problem indistinguishable from a video one.
        const float aspect = view.height > 0
                                 ? static_cast<float>(view.width) / static_cast<float>(view.height)
                                 : 1.0f;
        const float fovY = 0.8f;
        const float fov[4] = { -fovY * aspect, fovY * aspect, fovY, -fovY };

        const float identityPosition[3] = { eyeIndex == 0 ? -0.032f : 0.032f, 0.0f, 0.0f };
        const float identityOrientation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

        float projection[16];
        float viewMatrix[16];
        ProjectionFromFov(fov, 0.05f, 100.0f, projection);
        ViewFromPose(identityPosition, identityOrientation, viewMatrix);
        MultiplyMatrices(projection, viewMatrix, params.viewProjection);
    }

    params.params[0] = static_cast<float>(frame.timeSeconds);
    params.params[1] = static_cast<float>(eyeIndex);

    // Button bits are decoded here into a small integer the shader can unpack with plain
    // arithmetic, keeping bit manipulation out of HLSL where it reads badly and behaves
    // differently across compilers.
    for (int hand = 0; hand < 2; ++hand)
    {
        const xvr::ControllerInput& controller = frame.controllers[hand];

        params.handPosition[hand][0] = controller.gripPosition[0];
        params.handPosition[hand][1] = controller.gripPosition[1];
        params.handPosition[hand][2] = controller.gripPosition[2];
        params.handPosition[hand][3] = controller.active ? 1.0f : 0.0f;

        for (int i = 0; i < 4; ++i)
        {
            params.handOrientation[hand][i] = controller.gripOrientation[i];
        }

        params.handAnalog[hand][0] = controller.trigger;
        params.handAnalog[hand][1] = controller.squeeze;
        params.handAnalog[hand][2] = controller.thumbstick[0];
        params.handAnalog[hand][3] = controller.thumbstick[1];

        uint32_t packed = 0;
        packed |= (controller.buttons & xvr::net::Button_PrimaryClick) ? 1u : 0u;
        packed |= (controller.buttons & xvr::net::Button_SecondaryClick) ? 2u : 0u;
        packed |= (controller.buttons & xvr::net::Button_MenuClick) ? 4u : 0u;
        packed |= (controller.buttons & xvr::net::Button_ThumbstickClick) ? 8u : 0u;

        params.handButtons[hand][0] = static_cast<float>(packed);
        params.handButtons[hand][1] = 0.0f;
        params.handButtons[hand][2] = 0.0f;
        params.handButtons[hand][3] = 0.0f;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    XVR_CHECK(m_context->Map(m_constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    std::memcpy(mapped.pData, &params, sizeof(params));
    m_context->Unmap(m_constants.get(), 0);

    ID3D11RenderTargetView* rtv = view.renderTarget;
    m_context->OMSetRenderTargets(1, &rtv, view.depthStencil);
    m_context->OMSetDepthStencilState(m_depthState.get(), 0);
    m_context->RSSetViewports(1, &view.viewport);
    m_context->RSSetState(m_rasterizer.get());

    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(nullptr);

    ID3D11Buffer* constants = m_constants.get();
    m_context->VSSetShader(m_vertexShader.get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, &constants);
    m_context->PSSetShader(m_pixelShader.get(), nullptr, 0);

    m_context->Draw(kTotalVertices, 0);
}

} // namespace sample
