#include "TextDisplay.h"

#include "xvr/Check.h"

#include <d2d1_1helper.h>

// windows.h defines DrawText as DrawTextW, which shadows ID2D1RenderTarget::DrawText.
#ifdef DrawText
#undef DrawText
#endif

using winrt::com_ptr;

namespace xvr::probe {

void TextDisplay::Initialize(ID3D11Device* device, const winrt::Windows::UI::Core::CoreWindow& window)
{
    const auto bounds = window.Bounds();
    m_width = bounds.Width;
    m_height = bounds.Height;

    com_ptr<IDXGIDevice1> dxgiDevice;
    XVR_CHECK(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));

    com_ptr<IDXGIAdapter> adapter;
    XVR_CHECK(dxgiDevice->GetAdapter(adapter.put()));

    com_ptr<IDXGIFactory2> factory;
    XVR_CHECK(adapter->GetParent(IID_PPV_ARGS(factory.put())));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(m_width);
    desc.Height = static_cast<UINT>(m_height);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    XVR_CHECK(factory->CreateSwapChainForCoreWindow(device, winrt::get_unknown(window), &desc,
                                                    nullptr, m_swapChain.put()));

    D2D1_FACTORY_OPTIONS options{};
    com_ptr<ID2D1Factory1> d2dFactory;
    XVR_CHECK(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                  &options, d2dFactory.put_void()));

    com_ptr<ID2D1Device> d2dDevice;
    XVR_CHECK(d2dFactory->CreateDevice(dxgiDevice.get(), d2dDevice.put()));
    XVR_CHECK(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2dContext.put()));

    com_ptr<IDXGISurface> backBuffer;
    XVR_CHECK(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put())));

    const auto bitmapProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

    XVR_CHECK(m_d2dContext->CreateBitmapFromDxgiSurface(backBuffer.get(), &bitmapProperties,
                                                        m_target.put()));
    m_d2dContext->SetTarget(m_target.get());

    XVR_CHECK(m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.40f, 0.95f, 0.55f),
                                                  m_headlineBrush.put()));
    XVR_CHECK(m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.88f, 0.92f),
                                                  m_detailBrush.put()));

    com_ptr<IDWriteFactory> writeFactory;
    XVR_CHECK(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                    reinterpret_cast<IUnknown**>(writeFactory.put())));

    XVR_CHECK(writeFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             32.0f, L"en-us", m_headlineFormat.put()));
    XVR_CHECK(writeFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             17.0f, L"en-us", m_detailFormat.put()));
}

void TextDisplay::Draw(const std::vector<std::wstring>& headline,
                       const std::vector<std::wstring>& detail)
{
    // Drawing before a successful Initialize would dereference null COM pointers.
    // That turns any earlier failure into a crash, hiding the thing that actually
    // went wrong - so this is a no-op rather than an assert.
    if (!m_d2dContext || !m_swapChain)
    {
        return;
    }

    // Xbox overscan: keep everything well inside the panel so nothing is clipped on a TV.
    const float marginX = m_width * 0.07f;
    const float marginY = m_height * 0.07f;

    std::wstring headlineText;
    for (const auto& line : headline)
    {
        headlineText += line;
        headlineText += L'\n';
    }

    std::wstring detailText;
    for (const auto& line : detail)
    {
        detailText += line;
        detailText += L'\n';
    }

    m_d2dContext->BeginDraw();
    m_d2dContext->Clear(D2D1::ColorF(0.05f, 0.06f, 0.09f));

    const float headlineHeight = 40.0f * static_cast<float>(headline.size()) + 20.0f;

    m_d2dContext->DrawText(headlineText.c_str(), static_cast<UINT32>(headlineText.size()),
                           m_headlineFormat.get(),
                           D2D1::RectF(marginX, marginY, m_width - marginX, m_height - marginY),
                           m_headlineBrush.get());

    m_d2dContext->DrawText(detailText.c_str(), static_cast<UINT32>(detailText.size()),
                           m_detailFormat.get(),
                           D2D1::RectF(marginX, marginY + headlineHeight, m_width - marginX,
                                       m_height - marginY),
                           m_detailBrush.get());

    const HRESULT hr = m_d2dContext->EndDraw();
    if (FAILED(hr))
    {
        LogError(L"D2D EndDraw failed: 0x{:08X}", static_cast<uint32_t>(hr));
    }

    m_swapChain->Present(1, 0);
}

} // namespace xvr::probe
