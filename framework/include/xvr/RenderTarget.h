#pragma once

#include "xvr/StereoRenderer.h"

#include <cstdint>

#include <d3d11_4.h>

#include <winrt/base.h>

namespace xvr {

// A single eye's colour target.
//
// Each eye gets its own texture rather than sharing one side-by-side surface, because
// the encoder's throughput budget applies per instance: one encoder per eye is what
// buys full per-eye resolution. See docs/xbox-encoder-constraints.md.
class RenderTarget
{
public:
    void Initialize(ID3D11Device* device, uint32_t width, uint32_t height);

    ID3D11Texture2D* Texture() const { return m_texture.get(); }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

    // The view handed to IStereoRenderer for this eye.
    EyeView View() const;

    void Clear(ID3D11DeviceContext* context, const float rgba[4]) const;

private:
    winrt::com_ptr<ID3D11Texture2D> m_texture;
    winrt::com_ptr<ID3D11RenderTargetView> m_rtv;
    winrt::com_ptr<ID3D11Texture2D> m_depthTexture;
    winrt::com_ptr<ID3D11DepthStencilView> m_dsv;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

} // namespace xvr
