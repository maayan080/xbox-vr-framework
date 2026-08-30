#include "xvr/D3DDevice.h"

#include "xvr/Check.h"
#include "xvr/Log.h"

namespace xvr {

void D3DDevice::Initialize()
{
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                     ARRAYSIZE(levels), D3D11_SDK_VERSION, m_device.put(),
                                     &m_featureLevel, m_context.put());

    if (FAILED(hr))
    {
        // Without VIDEO_SUPPORT there is no video processor, so NV12 conversion cannot
        // work - but failing here with a clear log beats failing cryptically later.
        LogError(L"Device creation with VIDEO_SUPPORT failed: 0x{:08X}", static_cast<uint32_t>(hr));
        XVR_CHECK(hr);
    }

    // The encoder MFT calls into the device from its own threads.
    winrt::com_ptr<ID3D11Multithread> multithread;
    if (SUCCEEDED(m_context->QueryInterface(IID_PPV_ARGS(multithread.put()))))
    {
        multithread->SetMultithreadProtected(TRUE);
    }
    else
    {
        LogWarn(L"ID3D11Multithread unavailable; encoder threading may be unsafe");
    }

    LogInfo(L"D3D11 device created, feature level 0x{:04X}", static_cast<uint32_t>(m_featureLevel));
}

} // namespace xvr
