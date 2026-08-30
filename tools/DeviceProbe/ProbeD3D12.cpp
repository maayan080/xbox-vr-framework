// D3D12 Video Encode capability probe.
//
// This exists to separate two very different explanations for Media Foundation
// rejecting everything above 1920x1080 on Xbox Dev Mode:
//
//   (a) the hardware encode block genuinely cannot do more, or
//   (b) Media Foundation's capture-oriented layer is imposing a limit the
//       silicon does not have.
//
// D3D12 talks to the encode block directly, and reports its own maximum profile
// and level. An H.264 max level of 5.1 means the hardware does 4K regardless of
// what MF will accept - which would make (b) the answer, and would mean a D3D12
// encode path buys us resolutions MF refuses.

#include "ProbeInternal.h"

#include <d3d12.h>
#include <d3d12video.h>

#include <format>

using winrt::com_ptr;

namespace xvr::probe {
namespace {

// Maximum luma samples per frame for each H.264 level (Table A-1). Used to turn
// a reported max level into the practical answer: what frame sizes fit.
struct LevelInfo
{
    const wchar_t* name;
    uint32_t maxFrameSamples;
};

constexpr LevelInfo kH264Levels[] = {
    { L"1",   25344 },   { L"1b",  25344 },   { L"1.1", 101376 },  { L"1.2", 101376 },
    { L"1.3", 101376 },  { L"2",   101376 },  { L"2.1", 202752 },  { L"2.2", 414720 },
    { L"3",   414720 },  { L"3.1", 921600 },  { L"3.2", 1310720 }, { L"4",   2097152 },
    { L"4.1", 2097152 }, { L"4.2", 2228224 }, { L"5",   5652480 }, { L"5.1", 9437184 },
    { L"5.2", 9437184 }, { L"6",   35651584 },{ L"6.1", 35651584 },{ L"6.2", 35651584 },
};

const wchar_t* kH264ProfileNames[] = { L"Main", L"High", L"High 10" };

void DescribeLevel(Report& report, uint32_t levelIndex)
{
    if (levelIndex >= ARRAYSIZE(kH264Levels))
    {
        AddLine(report, std::format(L"    Max level         : <unknown index {}>", levelIndex));
        return;
    }

    const LevelInfo& level = kH264Levels[levelIndex];
    AddLine(report, std::format(L"    Max level         : {} ({} luma samples/frame)", level.name,
                                level.maxFrameSamples));

    // Translate the abstract limit into the frame sizes this project cares about.
    for (const auto& [label, w, h] : { std::tuple{ L"1920x1080 (960/eye SBS)", 1920u, 1080u },
                                       std::tuple{ L"2880x1584 (1440/eye SBS)", 2880u, 1584u },
                                       std::tuple{ L"3840x1080 (1920/eye SBS)", 3840u, 1080u },
                                       std::tuple{ L"3840x2160 (1920/eye SBS)", 3840u, 2160u } })
    {
        const uint32_t samples = w * h;
        AddLine(report, std::format(L"      {:<26} {}", label,
                                    samples <= level.maxFrameSamples ? L"fits" : L"EXCEEDS level"));
    }
}

void ProbeCodec(Report& report, ID3D12VideoDevice* videoDevice, D3D12_VIDEO_ENCODER_CODEC codec,
                const wchar_t* codecName)
{
    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC codecSupport{};
    codecSupport.NodeIndex = 0;
    codecSupport.Codec = codec;

    HRESULT hr = videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_CODEC, &codecSupport,
                                                  sizeof(codecSupport));
    if (FAILED(hr))
    {
        AddLine(report, std::format(L"  {} codec query failed: {}", codecName, HrText(hr)));
        return;
    }

    AddLine(report, std::format(L"  {} encode supported : {}", codecName,
                                codecSupport.IsSupported ? L"YES" : L"no"));

    if (!codecSupport.IsSupported || codec != D3D12_VIDEO_ENCODER_CODEC_H264)
    {
        return;
    }

    for (uint32_t profileIndex = 0; profileIndex < ARRAYSIZE(kH264ProfileNames); ++profileIndex)
    {
        auto profile = static_cast<D3D12_VIDEO_ENCODER_PROFILE_H264>(profileIndex);
        auto minLevel = D3D12_VIDEO_ENCODER_LEVELS_H264_1;
        auto maxLevel = D3D12_VIDEO_ENCODER_LEVELS_H264_1;

        D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL levelSupport{};
        levelSupport.NodeIndex = 0;
        levelSupport.Codec = codec;
        levelSupport.Profile.DataSize = sizeof(profile);
        levelSupport.Profile.pH264Profile = &profile;
        levelSupport.MinSupportedLevel.DataSize = sizeof(minLevel);
        levelSupport.MinSupportedLevel.pH264LevelSetting = &minLevel;
        levelSupport.MaxSupportedLevel.DataSize = sizeof(maxLevel);
        levelSupport.MaxSupportedLevel.pH264LevelSetting = &maxLevel;

        hr = videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL,
                                              &levelSupport, sizeof(levelSupport));

        if (FAILED(hr))
        {
            AddLine(report, std::format(L"  H.264 {} profile   : query failed ({})",
                                        kH264ProfileNames[profileIndex], HrText(hr)));
            continue;
        }

        AddLine(report, std::format(L"  H.264 {} profile   : {}", kH264ProfileNames[profileIndex],
                                    levelSupport.IsSupported ? L"supported" : L"not supported"));

        if (levelSupport.IsSupported)
        {
            DescribeLevel(report, static_cast<uint32_t>(maxLevel));
        }
    }
}

} // namespace

void ProbeD3D12Encode(Report& report)
{
    AddLine(report, L"");
    AddLine(report, L"=== D3D12 Video Encode ===");

    com_ptr<ID3D12Device> device;
    HRESULT hr = ::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put()));
    if (FAILED(hr))
    {
        AddLine(report, std::format(L"D3D12CreateDevice failed: {}", HrText(hr)));
        report.summary.push_back(L"D3D12 encode: device unavailable");
        return;
    }

    com_ptr<ID3D12VideoDevice> videoDevice;
    hr = device->QueryInterface(IID_PPV_ARGS(videoDevice.put()));
    if (FAILED(hr))
    {
        AddLine(report, std::format(L"ID3D12VideoDevice unavailable: {}", HrText(hr)));
        report.summary.push_back(L"D3D12 encode: NOT exposed");
        return;
    }

    AddLine(report, L"ID3D12VideoDevice     : available");

    ProbeCodec(report, videoDevice.get(), D3D12_VIDEO_ENCODER_CODEC_H264, L"H.264");
    ProbeCodec(report, videoDevice.get(), D3D12_VIDEO_ENCODER_CODEC_HEVC, L"HEVC");
}

} // namespace xvr::probe
