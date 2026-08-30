#pragma once

#include "xvr/Constraints.h"

#include <string>
#include <vector>

#include <d3d11_4.h>

namespace sample {

struct RunResult
{
    uint32_t encoderCount = 0;
    xvr::FrameConfig config;
    uint64_t framesSubmitted = 0;
    uint64_t framesEncoded = 0;
    uint64_t bytesEncoded = 0;
    double wallSeconds = 0.0;
    double achievedFps = 0.0;
    bool sustainedTarget = false;
    double latencyMinMs = 0.0;
    double latencyAvgMs = 0.0;
    double latencyMaxMs = 0.0;
};

struct TestReport
{
    std::vector<std::wstring> lines;
    std::vector<std::wstring> summary;
    // Runs for the primary configuration, used for the scaling verdict.
    std::vector<RunResult> runs;
    // Every run, across all configurations tested.
    std::vector<RunResult> allRuns;
};

// Runs the render -> NV12 -> encode path and measures sustained frame rate at 1 and 2
// encoder instances, then writes a playable H.264 file so the output can be verified.
//
// `outputDirectory` receives the .h264 dumps; pass an empty string to skip writing them.
TestReport Run(ID3D11Device* device, ID3D11DeviceContext* context,
               const std::wstring& outputDirectory);

} // namespace sample
