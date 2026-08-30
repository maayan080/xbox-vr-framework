#pragma once

#include <d3d11_4.h>

#include <winrt/base.h>

namespace xvr {

// The framework's D3D11 device.
//
// Created with VIDEO_SUPPORT (needed for the NV12 conversion the encoder requires) and
// with multithread protection enabled, because Media Foundation's encoder MFT touches
// the device from its own threads.
class D3DDevice
{
public:
    void Initialize();

    ID3D11Device* Device() const { return m_device.get(); }
    ID3D11DeviceContext* Context() const { return m_context.get(); }
    D3D_FEATURE_LEVEL FeatureLevel() const { return m_featureLevel; }

private:
    winrt::com_ptr<ID3D11Device> m_device;
    winrt::com_ptr<ID3D11DeviceContext> m_context;
    D3D_FEATURE_LEVEL m_featureLevel{};
};

} // namespace xvr
