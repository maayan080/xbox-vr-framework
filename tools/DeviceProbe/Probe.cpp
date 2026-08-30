#include "ProbeInternal.h"

#include "xvr/Log.h"

#include <codecapi.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <mferror.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <winrt/Windows.Security.ExchangeActiveSyncProvisioning.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.System.h>

#include <format>
#include <vector>

using winrt::com_ptr;

namespace xvr::probe {
namespace {

constexpr double kMiB = 1024.0 * 1024.0;

struct TestCase
{
    std::wstring label;
    GUID subtype;
    UINT32 width;
    UINT32 height;
    UINT32 fps;
    UINT32 bitrateBps;
    UINT32 profile; // 0 = leave unset
};

// One activated encoder, torn down correctly on destruction. Held separately from
// the configure step so the concurrency test can keep several alive at once.
struct EncoderInstance
{
    com_ptr<IMFActivate> activate;
    com_ptr<IMFTransform> transform;

    ~EncoderInstance()
    {
        if (activate)
        {
            activate->ShutdownObject();
            activate->DetachObject();
        }
    }
};

com_ptr<IMFActivate> FreshActivate(const GUID& subtype, UINT32 flags, UINT32 index)
{
    MFT_REGISTER_TYPE_INFO outputInfo{ MFMediaType_Video, subtype };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(::MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outputInfo, &activates,
                           &count)))
    {
        return nullptr;
    }

    com_ptr<IMFActivate> result;
    for (UINT32 i = 0; i < count; ++i)
    {
        if (i == index)
        {
            result.copy_from(activates[i]);
        }
        activates[i]->Release();
    }

    if (activates)
    {
        ::CoTaskMemFree(activates);
    }
    return result;
}

std::wstring ActivateName(IMFActivate* activate)
{
    WCHAR* name = nullptr;
    UINT32 length = 0;
    if (SUCCEEDED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length)) && name)
    {
        std::wstring result(name);
        ::CoTaskMemFree(name);
        return result;
    }
    return L"(unnamed MFT)";
}

// Activates and fully configures one encoder. Every configuration gets its own
// freshly enumerated activate object, so a failure in one test cannot leave state
// behind that makes later tests report failures they do not actually have.
HRESULT Configure(EncoderInstance& instance, ID3D11Device* device, const TestCase& test,
                  UINT32 enumFlags, UINT32 encoderIndex, std::wstring& stage)
{
    stage = L"enumerate";
    instance.activate = FreshActivate(test.subtype, enumFlags, encoderIndex);
    if (!instance.activate)
    {
        return E_FAIL;
    }

    stage = L"ActivateObject";
    HRESULT hr = instance.activate->ActivateObject(IID_PPV_ARGS(instance.transform.put()));
    if (FAILED(hr))
    {
        return hr;
    }

    com_ptr<IMFAttributes> attributes;
    instance.transform->GetAttributes(attributes.put());

    UINT32 isAsync = 0;
    UINT32 isD3DAware = 0;
    if (attributes)
    {
        attributes->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
        attributes->GetUINT32(MF_SA_D3D11_AWARE, &isD3DAware);
        if (isAsync)
        {
            attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        }
    }

    if (isD3DAware && device)
    {
        stage = L"SET_D3D_MANAGER";
        com_ptr<IMFDXGIDeviceManager> manager;
        UINT token = 0;
        hr = ::MFCreateDXGIDeviceManager(&token, manager.put());
        if (FAILED(hr))
        {
            return hr;
        }
        hr = manager->ResetDevice(device, token);
        if (FAILED(hr))
        {
            return hr;
        }
        hr = instance.transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                reinterpret_cast<ULONG_PTR>(manager.get()));
        if (FAILED(hr))
        {
            return hr;
        }
    }

    stage = L"SetOutputType";
    com_ptr<IMFMediaType> outputType;
    hr = ::MFCreateMediaType(outputType.put());
    if (FAILED(hr))
    {
        return hr;
    }

    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, test.subtype);
    outputType->SetUINT32(MF_MT_AVG_BITRATE, test.bitrateBps);
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (test.profile != 0)
    {
        outputType->SetUINT32(MF_MT_MPEG2_PROFILE, test.profile);
    }
    ::MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, test.width, test.height);
    ::MFSetAttributeRatio(outputType.get(), MF_MT_FRAME_RATE, test.fps, 1);
    ::MFSetAttributeRatio(outputType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = instance.transform->SetOutputType(0, outputType.get(), 0);
    if (FAILED(hr))
    {
        return hr;
    }

    stage = L"SetInputType(NV12)";
    com_ptr<IMFMediaType> inputType;
    hr = ::MFCreateMediaType(inputType.put());
    if (FAILED(hr))
    {
        return hr;
    }

    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    ::MFSetAttributeSize(inputType.get(), MF_MT_FRAME_SIZE, test.width, test.height);
    ::MFSetAttributeRatio(inputType.get(), MF_MT_FRAME_RATE, test.fps, 1);
    ::MFSetAttributeRatio(inputType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = instance.transform->SetInputType(0, inputType.get(), 0);
    if (FAILED(hr))
    {
        return hr;
    }

    stage = L"ok";
    return S_OK;
}

// Lists every encoder of a given type and returns the index of the first one that
// actually activates.
//
// Index 0 is not safe to assume: a machine can advertise an encoder whose hardware
// isn't usable (a registered vendor MFT with no matching GPU behind it), and testing
// that one tells us nothing about the encoder we'd really use.
int SelectEncoder(Report& report, const GUID& subtype, UINT32 flags)
{
    MFT_REGISTER_TYPE_INFO outputInfo{ MFMediaType_Video, subtype };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(::MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outputInfo, &activates,
                           &count)) ||
        count == 0)
    {
        if (activates)
        {
            ::CoTaskMemFree(activates);
        }
        return -1;
    }

    int selected = -1;
    for (UINT32 i = 0; i < count; ++i)
    {
        const std::wstring name = ActivateName(activates[i]);

        com_ptr<IMFTransform> transform;
        const HRESULT hr = activates[i]->ActivateObject(IID_PPV_ARGS(transform.put()));
        const bool usable = SUCCEEDED(hr);
        if (usable)
        {
            transform = nullptr;
            activates[i]->ShutdownObject();
        }
        activates[i]->DetachObject();

        AddLine(report, std::format(L"  [{}] {}{}", i, name,
                                    usable ? L"" : std::format(L"  (will not activate: {})",
                                                               HrText(hr))));

        if (usable && selected < 0)
        {
            selected = static_cast<int>(i);
        }

        activates[i]->Release();
    }

    ::CoTaskMemFree(activates);

    if (selected >= 0)
    {
        AddLine(report, std::format(L"  -> testing encoder [{}]", selected));
    }
    return selected;
}

void RunGroup(Report& report, ID3D11Device* device, const wchar_t* groupName,
              const std::vector<TestCase>& tests, UINT32 enumFlags, UINT32 encoderIndex)
{
    AddLine(report, std::format(L"  -- {} --", groupName));

    for (const auto& test : tests)
    {
        // A driver that faults on an unsupported configuration must not cost us the
        // results already gathered. Each case is contained.
        try
        {
            EncoderInstance instance;
            std::wstring stage;
            const HRESULT hr = Configure(instance, device, test, enumFlags, encoderIndex, stage);

            if (SUCCEEDED(hr))
            {
                AddLine(report, std::format(L"     OK      {}", test.label));
            }
            else
            {
                AddLine(report, std::format(L"     FAILED  {}  (at {}: {})", test.label, stage,
                                            HrText(hr)));
            }
        }
        catch (const winrt::hresult_error& e)
        {
            AddLine(report, std::format(L"     THREW   {}  ({})", test.label,
                                        HrText(e.code())));
        }
        catch (...)
        {
            AddLine(report, std::format(L"     THREW   {}  (unknown exception)", test.label));
        }
    }
}

// The question that decides the architecture if the per-session cap is real:
// can we run one encoder per eye at full 1080p, instead of splitting a single
// 1080p frame side-by-side and getting 960 per eye?
void ProbeConcurrency(Report& report, ID3D11Device* device, UINT32 enumFlags, UINT32 encoderIndex,
                      const TestCase& test, const wchar_t* description)
{
    AddLine(report, L"");
    AddLine(report, std::format(L"=== Concurrent H.264 encoder instances ({}) ===", description));

    for (UINT32 wanted = 1; wanted <= 4; ++wanted)
    {
        std::vector<std::unique_ptr<EncoderInstance>> instances;
        UINT32 succeeded = 0;
        std::wstring firstFailure;

        for (UINT32 i = 0; i < wanted; ++i)
        {
            auto instance = std::make_unique<EncoderInstance>();
            std::wstring stage;

            const HRESULT hr = Configure(*instance, device, test, enumFlags, encoderIndex, stage);
            if (SUCCEEDED(hr))
            {
                ++succeeded;
                instances.push_back(std::move(instance));
            }
            else if (firstFailure.empty())
            {
                firstFailure = std::format(L"at {}: {}", stage, HrText(hr));
            }
        }

        AddLine(report, std::format(L"  requested {}  ->  {} configured{}", wanted, succeeded,
                                    firstFailure.empty() ? L""
                                                         : std::format(L"  ({})", firstFailure)));

        if (wanted == 2)
        {
            report.summary.push_back(std::format(L"2x concurrent {}: {}", description,
                                                 succeeded >= 2 ? L"YES" : L"no"));
        }
    }
}

void ProbeSystem(Report& report)
{
    AddLine(report, L"=== System ===");

    try
    {
        const auto info = winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo();
        AddLine(report, std::format(L"DeviceFamily        : {}", std::wstring(info.DeviceFamily())));

        const std::wstring versionText(info.DeviceFamilyVersion());
        const unsigned long long packed = std::wcstoull(versionText.c_str(), nullptr, 10);
        AddLine(report, std::format(L"OS build            : {}.{}.{}.{}", (packed >> 48) & 0xFFFF,
                                    (packed >> 32) & 0xFFFF, (packed >> 16) & 0xFFFF,
                                    packed & 0xFFFF));
    }
    catch (...)
    {
        AddLine(report, L"DeviceFamily        : <query failed>");
    }

    try
    {
        const winrt::Windows::Security::ExchangeActiveSyncProvisioning::EasClientDeviceInformation eas;
        AddLine(report, std::format(L"Product             : {}", std::wstring(eas.SystemProductName())));
        AddLine(report, std::format(L"SKU                 : {}", std::wstring(eas.SystemSku())));
        report.summary.push_back(std::format(L"{}", std::wstring(eas.SystemProductName())));
    }
    catch (...)
    {
        AddLine(report, L"EAS device info     : <query failed>");
    }
}

void ProbeMemory(Report& report)
{
    AddLine(report, L"");
    AddLine(report, L"=== Memory budget ===");

    try
    {
        using winrt::Windows::System::MemoryManager;
        const auto limit = MemoryManager::AppMemoryUsageLimit();
        AddLine(report, std::format(L"AppMemoryUsageLimit : {:.1f} MiB", limit / kMiB));
        AddLine(report, std::format(L"AppMemoryUsage      : {:.1f} MiB",
                                    MemoryManager::AppMemoryUsage() / kMiB));
    }
    catch (...)
    {
        AddLine(report, L"MemoryManager       : <query failed>");
    }
}

void ProbeGpu(Report& report, ID3D11Device* device)
{
    AddLine(report, L"");
    AddLine(report, L"=== GPU ===");

    AddLine(report, std::format(L"D3D feature level   : 0x{:04X}",
                                static_cast<uint32_t>(device->GetFeatureLevel())));

    com_ptr<ID3D11VideoDevice> videoDevice;
    AddLine(report, std::format(L"ID3D11VideoDevice   : {}",
                                SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(videoDevice.put())))
                                    ? L"available"
                                    : L"NOT available"));

    com_ptr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()))))
    {
        com_ptr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(adapter.put())))
        {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc)))
            {
                AddLine(report, std::format(L"Adapter             : {}", desc.Description));
                AddLine(report, std::format(L"Dedicated video mem : {:.1f} MiB",
                                            desc.DedicatedVideoMemory / kMiB));
                AddLine(report, std::format(L"Shared system mem   : {:.1f} MiB",
                                            desc.SharedSystemMemory / kMiB));
            }
        }
    }
}

std::vector<TestCase> ResolutionSweep(const GUID& subtype, UINT32 profile)
{
    // Deliberately includes sizes BELOW the likely display mode. If 1280x720 fails
    // while 1920x1080 passes, the encoder is locked to exactly the display mode; if
    // both pass, it is a maximum, and we have freedom underneath it.
    const UINT32 sizes[][2] = {
        { 1280, 720 },  { 1600, 900 },  { 1920, 1080 }, { 1920, 1088 }, { 2048, 1088 },
        { 2560, 1440 }, { 2880, 1584 }, { 3840, 1080 }, { 3840, 2160 },
    };

    std::vector<TestCase> tests;
    for (const auto& size : sizes)
    {
        tests.push_back({ std::format(L"{}x{}", size[0], size[1]), subtype, size[0], size[1], 60,
                          20'000'000, profile });
    }
    return tests;
}

} // namespace

void AddLine(Report& report, std::wstring line)
{
    // Written to the log immediately, not batched until the end of the run. The log
    // flushes per line, so if the probe dies mid-way the log still shows exactly how
    // far it got - which is the whole point of having one.
    LogRaw(LogLevel::Info, line);
    report.lines.push_back(std::move(line));
}

std::wstring HrText(HRESULT hr)
{
    switch (hr)
    {
    case S_OK:                        return L"S_OK";
    case MF_E_INVALIDMEDIATYPE:       return L"MF_E_INVALIDMEDIATYPE";
    case MF_E_INVALIDTYPE:            return L"MF_E_INVALIDTYPE";
    case MF_E_TRANSFORM_TYPE_NOT_SET: return L"MF_E_TRANSFORM_TYPE_NOT_SET";
    case MF_E_UNSUPPORTED_D3D_TYPE:   return L"MF_E_UNSUPPORTED_D3D_TYPE";
    case MF_E_INVALIDREQUEST:         return L"MF_E_INVALIDREQUEST";
    case E_NOTIMPL:                   return L"E_NOTIMPL";
    case E_INVALIDARG:                return L"E_INVALIDARG";
    case E_OUTOFMEMORY:               return L"E_OUTOFMEMORY";
    case E_FAIL:                      return L"E_FAIL";
    default:                          return std::format(L"0x{:08X}", static_cast<uint32_t>(hr));
    }
}

Report Run(ID3D11Device* device)
{
    Report report;

    AddLine(report, L"Xbox VR device capability probe (v2)");
    AddLine(report, L"=====================================");

    // Sections are independent: a failure in one must not cost us the others, since
    // every one of them is a separate question we came here to answer.
    const auto section = [&report](const wchar_t* name, auto&& body) {
        try
        {
            body();
        }
        catch (const winrt::hresult_error& e)
        {
            AddLine(report, std::format(L"[{}] section failed: {}", name, HrText(e.code())));
        }
        catch (...)
        {
            AddLine(report, std::format(L"[{}] section failed with an unknown exception", name));
        }
    };

    section(L"System", [&] { ProbeSystem(report); });
    section(L"Memory", [&] { ProbeMemory(report); });
    section(L"GPU", [&] { ProbeGpu(report, device); });
    section(L"Display", [&] { ProbeDisplay(report); });
    section(L"D3D12", [&] { ProbeD3D12Encode(report); });

    const HRESULT startup = ::MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(startup))
    {
        AddLine(report, std::format(L"MFStartup failed: {}", HrText(startup)));
        return report;
    }

    constexpr UINT32 kHardware = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
    constexpr UINT32 kSoftware =
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;

    AddLine(report, L"");
    AddLine(report, L"=== Hardware H.264 encoder ===");

    const int h264Index = SelectEncoder(report, MFVideoFormat_H264, kHardware);
    if (h264Index >= 0)
    {
        const auto index = static_cast<UINT32>(h264Index);
        report.hardwareH264Found = true;

        RunGroup(report, device, L"Resolution sweep (Main profile, 60fps, 20Mbps)",
                 ResolutionSweep(MFVideoFormat_H264, eAVEncH264VProfile_Main), kHardware, index);

        // High profile supports higher levels than Main. If the stereo resolutions
        // pass here but failed above, the limit was profile/level - not the display.
        RunGroup(report, device, L"Resolution sweep (High profile, 60fps, 20Mbps)",
                 ResolutionSweep(MFVideoFormat_H264, eAVEncH264VProfile_High), kHardware, index);

        std::vector<TestCase> fpsSweep;
        for (const UINT32 fps : { 30u, 60u, 72u, 90u, 120u })
        {
            fpsSweep.push_back({ std::format(L"1920x1080 @{}fps", fps), MFVideoFormat_H264, 1920,
                                 1080, fps, 20'000'000, eAVEncH264VProfile_Main });
        }
        RunGroup(report, device, L"Frame rate sweep (1920x1080, 20Mbps)", fpsSweep, kHardware, index);

        std::vector<TestCase> bitrateSweep;
        for (const UINT32 mbps : { 5u, 10u, 20u, 40u, 80u, 150u })
        {
            bitrateSweep.push_back({ std::format(L"{} Mbps", mbps), MFVideoFormat_H264, 1920, 1080,
                                     60, mbps * 1'000'000, eAVEncH264VProfile_Main });
        }
        RunGroup(report, device, L"Bitrate sweep (1920x1080 @60)", bitrateSweep, kHardware, index);

        // Is the 60fps ceiling a hard frame-rate cap, or a pixel-rate budget?
        //
        // If it is a budget (macroblocks/second rather than frames/second), smaller
        // frames should buy proportionally higher frame rates - 1920x1080@60 is about
        // 124 Mpix/s, which at 720p would allow roughly 135fps. That distinction is
        // worth real money: 720p@120 per eye halves latency versus 1080p@60, and for
        // a 640x480 source it may be the better trade.
        //
        // This is answerable on a plain 1080p60 display; no 120Hz panel required.
        std::vector<TestCase> rateBudget;
        for (const auto& [w, h] : { std::pair{ 1280u, 720u }, std::pair{ 1600u, 900u },
                                    std::pair{ 1920u, 1088u } })
        {
            for (const UINT32 fps : { 60u, 72u, 90u, 120u, 144u })
            {
                rateBudget.push_back({ std::format(L"{}x{} @{}fps", w, h, fps), MFVideoFormat_H264,
                                       w, h, fps, 20'000'000, eAVEncH264VProfile_Main });
            }
        }
        RunGroup(report, device, L"Pixel-rate budget: frame rate vs resolution", rateBudget,
                 kHardware, index);

        // Pin the throughput budget precisely at 72fps - the Quest's native refresh.
        //
        // Measurements so far only bracket it: 518,400 MB/s passes and 587,520 fails.
        // That gap is the difference between ~1792x1008 and ~1920x1072 per eye, so it
        // is worth resolving exactly rather than assuming the standard Level 4.2 value.
        //
        // All dimensions are multiples of 16 so no macroblock rounding is involved, and
        // the ladder climbs in small steps so the pass/fail boundary lands between two
        // adjacent rows. Every entry stays under 8192 MB/frame - see the crash note.
        const UINT32 ladder[][2] = {
            { 1600, 896 },  // 5,600 MB -> 403,200 MB/s
            { 1728, 960 },  // 6,480    -> 466,560
            { 1792, 1008 }, // 7,056    -> 508,032
            { 1808, 1008 }, // 7,119    -> 512,568
            { 1824, 1024 }, // 7,296    -> 525,312
            { 1856, 1024 }, // 7,424    -> 534,528
            { 1888, 1040 }, // 7,670    -> 552,240
            { 1920, 1056 }, // 7,920    -> 570,240
            { 1920, 1072 }, // 8,040    -> 578,880
        };

        std::vector<TestCase> ladder72;
        for (const auto& size : ladder)
        {
            const UINT32 macroblocks = (size[0] / 16) * (size[1] / 16);
            ladder72.push_back({ std::format(L"{}x{}  ({} MB -> {} MB/s)", size[0], size[1],
                                             macroblocks, macroblocks * 72),
                                 MFVideoFormat_H264, size[0], size[1], 72, 20'000'000,
                                 eAVEncH264VProfile_Main });
        }
        RunGroup(report, device, L"Maximum resolution at 72fps", ladder72, kHardware, index);

        // A VR eye view is much closer to square than 16:9. At identical encoder cost a
        // squarer frame covers more of the headset's field of view, so these are likely
        // better per-eye shapes than a wide letterbox with the same macroblock count.
        const UINT32 squares[][2] = {
            { 1024, 1024 }, // 4,096 MB -> 294,912 MB/s
            { 1216, 1216 }, // 5,776    -> 415,872
            { 1280, 1280 }, // 6,400    -> 460,800
            { 1344, 1344 }, // 7,056    -> 508,032
            { 1376, 1376 }, // 7,396    -> 532,512
            { 1408, 1408 }, // 7,744    -> 557,568
        };

        std::vector<TestCase> square72;
        for (const auto& size : squares)
        {
            const UINT32 macroblocks = (size[0] / 16) * (size[1] / 16);
            square72.push_back({ std::format(L"{}x{}  ({} MB -> {} MB/s)", size[0], size[1],
                                             macroblocks, macroblocks * 72),
                                 MFVideoFormat_H264, size[0], size[1], 72, 20'000'000,
                                 eAVEncH264VProfile_Main });
        }
        RunGroup(report, device, L"Square frames at 72fps (better VR coverage)", square72,
                 kHardware, index);

        // DO NOT test 2048x1024 here. That is exactly 2^21 luma samples (8192
        // macroblocks), and on Xbox Series S it passes media-type validation and then
        // hard-crashes the process inside the encoder - an access violation, not a
        // failed HRESULT, so it cannot be caught. Measured 2026-08-12.
        //
        // Frame sizes must stay strictly below 8192 macroblocks, never exactly at it.
        const std::vector<TestCase> boundary{
            { L"2032x1024  (8128 MB, just under)", MFVideoFormat_H264, 2032, 1024, 60, 20'000'000,
              eAVEncH264VProfile_Main },
            { L"1440x1440  (8100 MB, square SBS)", MFVideoFormat_H264, 1440, 1440, 60, 20'000'000,
              eAVEncH264VProfile_Main },
            { L"1280x1600  (8000 MB, tall SBS)", MFVideoFormat_H264, 1280, 1600, 60, 20'000'000,
              eAVEncH264VProfile_Main },
        };

        const std::vector<TestCase> profileSweep{
            { L"Baseline", MFVideoFormat_H264, 1920, 1080, 60, 20'000'000, eAVEncH264VProfile_Base },
            { L"Main", MFVideoFormat_H264, 1920, 1080, 60, 20'000'000, eAVEncH264VProfile_Main },
            { L"High", MFVideoFormat_H264, 1920, 1080, 60, 20'000'000, eAVEncH264VProfile_High },
            { L"unset", MFVideoFormat_H264, 1920, 1080, 60, 20'000'000, 0 },
        };
        RunGroup(report, device, L"Profile sweep (1920x1080 @60)", profileSweep, kHardware, index);

        // Two eyes at 1080p60 is the baseline architecture. Two eyes at 720p120 is the
        // alternative if the ceiling turns out to be a rate budget - testing both tells
        // us whether concurrent instances share that budget or each get their own.
        ProbeConcurrency(report, device, kHardware, index,
                         { L"", MFVideoFormat_H264, 1920, 1080, 60, 20'000'000,
                           eAVEncH264VProfile_Main },
                         L"1920x1080 @60");

        // 720p72 per eye fits the measured 522,240 MB/s budget for BOTH eyes at once
        // (518,400), and matches the Quest's native 72Hz exactly. If the budget turns
        // out to be shared, this is the configuration that survives.
        ProbeConcurrency(report, device, kHardware, index,
                         { L"", MFVideoFormat_H264, 1280, 720, 72, 20'000'000,
                           eAVEncH264VProfile_Main },
                         L"1280x720 @72");

        ProbeConcurrency(report, device, kHardware, index,
                         { L"", MFVideoFormat_H264, 1280, 720, 120, 20'000'000,
                           eAVEncH264VProfile_Main },
                         L"1280x720 @120");

        // Last, because frame-size edge cases are where the driver crashed before.
        RunGroup(report, device, L"Frame-size boundary", boundary, kHardware, index);
    }
    else
    {
        AddLine(report, L"No usable hardware H.264 encoder found.");
    }

    AddLine(report, L"");
    AddLine(report, L"=== Hardware HEVC encoder ===");
    const int hevcIndex = SelectEncoder(report, MFVideoFormat_HEVC, kHardware);
    if (hevcIndex >= 0)
    {
        RunGroup(report, device, L"Resolution sweep (60fps, 20Mbps)",
                 ResolutionSweep(MFVideoFormat_HEVC, 0), kHardware,
                 static_cast<UINT32>(hevcIndex));
    }
    else
    {
        AddLine(report, L"No usable hardware HEVC encoder found.");
    }

    // A software encoder has no display pipeline behind it. If it accepts the stereo
    // resolutions that hardware refused, the limit is specific to the hardware path.
    AddLine(report, L"");
    AddLine(report, L"=== Software H.264 encoder (control group) ===");
    const int softwareIndex = SelectEncoder(report, MFVideoFormat_H264, kSoftware);
    if (softwareIndex >= 0)
    {
        RunGroup(report, nullptr, L"Resolution sweep (60fps, 20Mbps)",
                 ResolutionSweep(MFVideoFormat_H264, eAVEncH264VProfile_Main), kSoftware,
                 static_cast<UINT32>(softwareIndex));
    }

    ::MFShutdown();
    AddLine(report, L"--- probe finished normally ---");

    return report;
}

} // namespace xvr::probe
