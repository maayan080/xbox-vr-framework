#include "xvr/RenderTarget.h"

#include "xvr/Check.h"

namespace xvr {

void RenderTarget::Initialize(ID3D11Device* device, uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    XVR_CHECK(device->CreateTexture2D(&desc, nullptr, m_texture.put()));
    XVR_CHECK(device->CreateRenderTargetView(m_texture.get(), nullptr, m_rtv.put()));

    D3D11_TEXTURE2D_DESC depthDesc = desc;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    XVR_CHECK(device->CreateTexture2D(&depthDesc, nullptr, m_depthTexture.put()));
    XVR_CHECK(device->CreateDepthStencilView(m_depthTexture.get(), nullptr, m_dsv.put()));
}

EyeView RenderTarget::View() const
{
    EyeView view;
    view.renderTarget = m_rtv.get();
    view.depthStencil = m_dsv.get();
    view.width = m_width;
    view.height = m_height;
    view.viewport = { 0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height),
                      0.0f, 1.0f };
    return view;
}

void RenderTarget::Clear(ID3D11DeviceContext* context, const float rgba[4]) const
{
    context->ClearRenderTargetView(m_rtv.get(), rgba);
    context->ClearDepthStencilView(m_dsv.get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
}

} // namespace xvr
