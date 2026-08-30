#pragma once

// Minimal Direct2D/DirectWrite text surface for the probe.
//
// The probe's real output is its log file, but that requires Device Portal to
// retrieve. Putting the headline results on screen means the console itself can
// answer "did this work?" without any tooling at all.

#include <string>
#include <vector>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>

#include <winrt/Windows.UI.Core.h>
#include <winrt/base.h>

namespace xvr::probe {

class TextDisplay
{
public:
    void Initialize(ID3D11Device* device, const winrt::Windows::UI::Core::CoreWindow& window);
    void Draw(const std::vector<std::wstring>& headline, const std::vector<std::wstring>& detail);

private:
    winrt::com_ptr<IDXGISwapChain1> m_swapChain;
    winrt::com_ptr<ID2D1DeviceContext> m_d2dContext;
    winrt::com_ptr<ID2D1Bitmap1> m_target;
    winrt::com_ptr<ID2D1SolidColorBrush> m_headlineBrush;
    winrt::com_ptr<ID2D1SolidColorBrush> m_detailBrush;
    winrt::com_ptr<IDWriteTextFormat> m_headlineFormat;
    winrt::com_ptr<IDWriteTextFormat> m_detailFormat;
    float m_width = 0.0f;
    float m_height = 0.0f;
};

} // namespace xvr::probe
