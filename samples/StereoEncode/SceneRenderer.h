#pragma once

#include "xvr/StereoRenderer.h"

#include <winrt/base.h>

namespace sample {

// The framework's reference content: a small 3D room rendered from the headset's reported
// viewpoint, with objects spread in depth so head motion produces visible parallax.
//
// Nothing here is known to the framework beyond IStereoRenderer - this is the seam other
// developers would replace with their own rendering.
class SceneRenderer : public xvr::IStereoRenderer
{
public:
    void Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;
    void RenderEye(const xvr::FrameContext& frame, xvr::Eye eye,
                   const xvr::EyeView& view) override;

private:
    struct SceneParams
    {
        float viewProjection[16];
        float params[4]; // x = time, y = eye index
        float handPosition[2][4];    // xyz grip position, w = active
        float handOrientation[2][4]; // grip orientation quaternion
        float handAnalog[2][4];      // trigger, squeeze, thumbstick x/y
        float handButtons[2][4];     // x = packed button flags as a small integer
    };

    ID3D11DeviceContext* m_context = nullptr;
    winrt::com_ptr<ID3D11VertexShader> m_vertexShader;
    winrt::com_ptr<ID3D11PixelShader> m_pixelShader;
    winrt::com_ptr<ID3D11Buffer> m_constants;
    winrt::com_ptr<ID3D11RasterizerState> m_rasterizer;
    winrt::com_ptr<ID3D11DepthStencilState> m_depthState;
};

} // namespace sample
