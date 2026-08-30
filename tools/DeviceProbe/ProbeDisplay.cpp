// Display configuration probe.
//
// If the encoder's accepted resolutions track the console's current display mode,
// then the display mode is a project-level constraint and we need to know exactly
// what it is, and what else the attached display would allow.
//
// HdmiDisplayInformation only reports modes the *connected display* advertises, so
// this cannot conjure 4K on a 1080p monitor. What it does tell us is whether an app
// can change the mode at all, and what the ceiling is for this particular setup.

#include "ProbeInternal.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Display.Core.h>

#include <format>

namespace xvr::probe {

void ProbeDisplay(Report& report)
{
    AddLine(report, L"");
    AddLine(report, L"=== Display configuration ===");

    try
    {
        using winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation;

        const auto hdmi = HdmiDisplayInformation::GetForCurrentView();
        if (!hdmi)
        {
            AddLine(report, L"HdmiDisplayInformation: not available on this device");
            return;
        }

        const auto current = hdmi.GetCurrentDisplayMode();
        if (current)
        {
            AddLine(report, std::format(L"Current mode        : {}x{} @{:.3f}Hz  {}bpp",
                                        current.ResolutionWidthInRawPixels(),
                                        current.ResolutionHeightInRawPixels(),
                                        current.RefreshRate(), current.BitsPerPixel()));
            report.summary.push_back(std::format(L"Display: {}x{} @{:.0f}Hz",
                                                 current.ResolutionWidthInRawPixels(),
                                                 current.ResolutionHeightInRawPixels(),
                                                 current.RefreshRate()));
        }

        const auto modes = hdmi.GetSupportedDisplayModes();
        AddLine(report, std::format(L"Supported modes     : {}", modes.Size()));

        // The full list matters: it is the set of encode resolutions available to
        // this user without buying hardware, if the display-mode theory holds.
        for (const auto& mode : modes)
        {
            AddLine(report, std::format(L"  {}x{} @{:.3f}Hz  {}bpp{}",
                                        mode.ResolutionWidthInRawPixels(),
                                        mode.ResolutionHeightInRawPixels(), mode.RefreshRate(),
                                        mode.BitsPerPixel(),
                                        mode.Is2086MetadataSupported() ? L"  [HDR10]" : L""));
        }
    }
    catch (const winrt::hresult_error& e)
    {
        AddLine(report, std::format(L"HdmiDisplayInformation failed: 0x{:08X}",
                                    static_cast<uint32_t>(e.code())));
    }
    catch (...)
    {
        AddLine(report, L"HdmiDisplayInformation: unexpected failure");
    }
}

} // namespace xvr::probe
