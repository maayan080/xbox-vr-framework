#include "Probe.h"
#include "TextDisplay.h"

#include "xvr/Check.h"
#include "xvr/Log.h"

#include <d3d11_4.h>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;

namespace {

com_ptr<ID3D11Device> CreateDevice()
{
    // BGRA_SUPPORT is required for the Direct2D readout; VIDEO_SUPPORT is what the
    // encode path will need, so failing to get it here is itself a probe result.
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL achieved{};

    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                     ARRAYSIZE(levels), D3D11_SDK_VERSION, device.put(), &achieved,
                                     context.put());

    if (FAILED(hr))
    {
        xvr::LogWarn(L"Device creation with VIDEO_SUPPORT failed (0x{:08X}); retrying without it",
                     static_cast<uint32_t>(hr));

        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                                 D3D11_SDK_VERSION, device.put(), &achieved, context.put());
    }

    XVR_CHECK(hr);
    return device;
}

struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
    IFrameworkView CreateView() { return *this; }
    void Initialize(const CoreApplicationView&) {}
    void SetWindow(const CoreWindow&) {}
    void Load(const hstring&) {}
    void Uninitialize() {}

    void Run()
    {
        xvr::LogInit(L"device-probe.log");
        xvr::LogInfo(L"Device probe starting");

        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();

        std::vector<std::wstring> headline;
        std::vector<std::wstring> detail;

        // The display is brought up BEFORE the probe runs. If the probe throws, the
        // failure has to be visible on screen - and it cannot be if the thing that
        // draws it was never initialised.
        com_ptr<ID3D11Device> device;
        try
        {
            device = CreateDevice();
            m_display.Initialize(device.get(), window);
            m_display.Draw({ L"Probing device capabilities..." }, {});
        }
        catch (const xvr::HresultException& e)
        {
            xvr::LogError(L"Display init failed: 0x{:08X}", static_cast<uint32_t>(e.Code()));
        }
        catch (...)
        {
            xvr::LogError(L"Display init failed with an unknown exception");
        }

        try
        {
            if (!device)
            {
                throw std::runtime_error("no D3D device");
            }

            const xvr::probe::Report report = xvr::probe::Run(device.get());
            headline = report.summary;
            detail = report.lines;

            xvr::LogInfo(L"Probe complete. Log written to {}", xvr::LogDirectory());
        }
        catch (const xvr::HresultException& e)
        {
            xvr::LogError(L"Probe aborted: 0x{:08X}", static_cast<uint32_t>(e.Code()));
            headline = { L"PROBE FAILED - see device-probe.log" };
        }
        catch (...)
        {
            xvr::LogError(L"Probe aborted with an unknown exception");
            headline = { L"PROBE FAILED - see device-probe.log" };
        }

        headline.push_back(L"");
        headline.push_back(L"Log: LocalState\\device-probe.log");

        CoreDispatcher dispatcher = window.Dispatcher();
        while (true)
        {
            dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            m_display.Draw(headline, detail);
        }
    }

    xvr::probe::TextDisplay m_display;
};

} // namespace

int __stdcall wWinMain(void*, void*, wchar_t*, int)
{
    CoreApplication::Run(make<App>());
    return 0;
}
