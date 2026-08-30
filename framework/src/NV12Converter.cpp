#include "xvr/NV12Converter.h"

#include "xvr/Check.h"
#include "xvr/Log.h"

namespace xvr {

void NV12Converter::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width,
                               uint32_t height, uint32_t poolSize)
{
    m_width = width;
    m_height = height;

    XVR_CHECK(device->QueryInterface(IID_PPV_ARGS(m_videoDevice.put())));
    XVR_CHECK(context->QueryInterface(IID_PPV_ARGS(m_videoContext.put())));

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc{};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = width;
    contentDesc.InputHeight = height;
    contentDesc.OutputWidth = width;
    contentDesc.OutputHeight = height;
    contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    XVR_CHECK(m_videoDevice->CreateVideoProcessorEnumerator(&contentDesc, m_enumerator.put()));
    XVR_CHECK(m_videoDevice->CreateVideoProcessor(m_enumerator.get(), 0, m_processor.put()));

    // Full-range RGB in, studio-range YUV out is the encoder's expectation; getting this
    // wrong shows up as washed-out or crushed contrast in the headset rather than an error.
    m_videoContext->VideoProcessorSetStreamColorSpace(m_processor.get(), 0, nullptr);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;

    // RENDER_TARGET is required so the video processor can write into these. Adding
    // VIDEO_ENCODER as well is preferable where supported, but Xbox rejects that
    // combination on NV12 with E_INVALIDARG, so it is probed once rather than assumed.
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
    {
        winrt::com_ptr<ID3D11Texture2D> probe;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, probe.put())))
        {
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            LogInfo(L"NV12 VIDEO_ENCODER bind unsupported; using RENDER_TARGET only");
        }
    }

    m_pool.resize(poolSize);
    for (auto& entry : m_pool)
    {
        XVR_CHECK(device->CreateTexture2D(&desc, nullptr, entry.texture.put()));

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        XVR_CHECK(m_videoDevice->CreateVideoProcessorOutputView(
            entry.texture.get(), m_enumerator.get(), &outputDesc, entry.outputView.put()));
    }

    LogInfo(L"NV12 converter ready: {}x{}, pool of {}", width, height, poolSize);
}

ID3D11Texture2D* NV12Converter::Convert(ID3D11Texture2D* source)
{
    PoolEntry& entry = m_pool[m_next];
    m_next = (m_next + 1) % m_pool.size();

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.FourCC = 0;
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDesc.Texture2D.MipSlice = 0;
    inputDesc.Texture2D.ArraySlice = 0;

    winrt::com_ptr<ID3D11VideoProcessorInputView> inputView;
    XVR_CHECK(m_videoDevice->CreateVideoProcessorInputView(source, m_enumerator.get(), &inputDesc,
                                                           inputView.put()));

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.get();

    XVR_CHECK(m_videoContext->VideoProcessorBlt(m_processor.get(), entry.outputView.get(), 0, 1,
                                                &stream));

    return entry.texture.get();
}

} // namespace xvr
