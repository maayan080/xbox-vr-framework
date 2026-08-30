#include "StreamMode.h"
#include "ThroughputTest.h"

#include "xvr/Check.h"
#include "xvr/D3DDevice.h"
#include "xvr/DebugTextDisplay.h"
#include "xvr/Log.h"

#include <chrono>
#include <thread>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;

namespace {

struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
    IFrameworkView CreateView() { return *this; }
    void Initialize(const CoreApplicationView&) {}
    void SetWindow(const CoreWindow&) {}
    void Load(const hstring&) {}
    void Uninitialize() {}

    void Run()
    {
        xvr::LogInit(L"stereo-encode.log");
        xvr::LogInfo(L"Stereo encode test starting");

        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();

        std::vector<std::wstring> headline;
        std::vector<std::wstring> detail;

        // Display first, so a failure in the test can be shown rather than crashing
        // the thing that would have displayed it.
        xvr::D3DDevice device;
        bool deviceReady = false;
        try
        {
            device.Initialize();
            deviceReady = true;
            m_display.Initialize(device.Device(), window);
            m_display.Draw({ L"Running encode throughput test...",
                             L"This takes about a minute." },
                           {});
        }
        catch (const xvr::HresultException& e)
        {
            xvr::LogError(L"Setup failed: 0x{:08X}", static_cast<uint32_t>(e.Code()));
        }
        catch (...)
        {
            xvr::LogError(L"Setup failed with an unknown exception");
        }

        // Streaming is the default now that the encode path is proven. The throughput
        // test remains in the build and can be switched back to when the encoder
        // configuration changes and needs re-measuring.
        sample::StreamSession session;
        bool streaming = false;

        try
        {
            if (!deviceReady)
            {
                throw std::runtime_error("no D3D device");
            }

            streaming = session.Start(device.Device(), device.Context());
            if (!streaming)
            {
                headline = { L"COULD NOT START - see stereo-encode.log" };
            }
        }
        catch (const xvr::HresultException& e)
        {
            xvr::LogError(L"Start failed: 0x{:08X}", static_cast<uint32_t>(e.Code()));
            headline = { L"START FAILED - see stereo-encode.log" };
        }
        catch (...)
        {
            xvr::LogError(L"Start failed with an unknown exception");
            headline = { L"START FAILED - see stereo-encode.log" };
        }

        CoreDispatcher dispatcher = window.Dispatcher();
        auto lastDraw = std::chrono::steady_clock::now();

        while (true)
        {
            dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);

            if (streaming)
            {
                try
                {
                    session.Tick(headline, detail);
                }
                catch (const xvr::HresultException& e)
                {
                    xvr::LogError(L"Stream tick failed: 0x{:08X}",
                                  static_cast<uint32_t>(e.Code()));
                    streaming = false;
                    headline = { L"STREAM FAILED - see stereo-encode.log" };
                }
            }

            // The readout is text and redraws a few times a second. Drawing it every
            // iteration would spend GPU time competing with the encoder for no benefit.
            const auto now = std::chrono::steady_clock::now();
            if (now - lastDraw > std::chrono::milliseconds(200))
            {
                lastDraw = now;
                m_display.Draw(headline, detail);
            }
            else
            {
                // Yield briefly so the pacing wait does not become a hot spin.
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    }

    xvr::DebugTextDisplay m_display;
};

} // namespace

int __stdcall wWinMain(void*, void*, wchar_t*, int)
{
    CoreApplication::Run(make<App>());
    return 0;
}
