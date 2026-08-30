#pragma once

// On-screen text for diagnostics on a console.
//
// Xbox has no console window and retrieving a log file needs Device Portal, so a tool
// that reports its results on the TV can be read immediately with no tooling at all.

#include <string>
#include <vector>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>

#include <winrt/Windows.UI.Core.h>
#include <winrt/base.h>

namespace xvr {

class DebugTextDisplay
{
public:
    void Initialize(ID3D11Device* device, const winrt::Windows::UI::Core::CoreWindow& window);

    // Safe to call before Initialize, or after it failed: draws nothing rather than
    // crashing, so an earlier failure stays visible instead of being replaced by a crash.
    void Draw(const std::vector<std::wstring>& headline, const std::vector<std::wstring>& detail);

    bool IsReady() const { return m_d2dContext && m_swapChain; }

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

} // namespace xvr
