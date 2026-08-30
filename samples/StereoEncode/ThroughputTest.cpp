#include "ThroughputTest.h"

#include "SceneRenderer.h"

#include "xvr/Check.h"
#include "xvr/EncodedFrameSink.h"
#include "xvr/Log.h"
#include "xvr/StereoEncodePipeline.h"

#include <chrono>
#include <format>
#include <thread>

namespace sample {
namespace {

constexpr uint64_t kMeasuredFrames = 300;
constexpr uint64_t kWarmupFrames = 30;

void Add(TestReport& report, std::wstring line)
{
    xvr::LogRaw(xvr::LogLevel::Info, line);
    report.lines.push_back(std::move(line));
}

// Runs one configuration and measures how fast it actually goes.
//
// Frames are submitted as fast as the encoder will accept them rather than paced to the
// target rate: pacing would measure the clock, not the hardware. The achieved rate is
// therefore the sustained ceiling.
RunResult RunOne(ID3D11Device* device, ID3D11DeviceContext* context,
                 const xvr::FrameConfig& config, uint32_t encoderCount,
                 const std::wstring& outputDirectory, TestReport& report, bool paced = false)
{
    RunResult result;
    result.encoderCount = encoderCount;
    result.config = config;

    SceneRenderer renderer;

    xvr::FileFrameSink leftFile;
    xvr::FileFrameSink rightFile;
    xvr::NullFrameSink leftNull;
    xvr::NullFrameSink rightNull;

    const bool writeFiles = !outputDirectory.empty();
    if (writeFiles)
    {
        leftFile.Open(std::format(L"{}\\eye0-{}x{}-{}enc.h264", outputDirectory, config.width,
                                  config.height, encoderCount));
        if (encoderCount > 1)
        {
            rightFile.Open(std::format(L"{}\\eye1-{}x{}-{}enc.h264", outputDirectory, config.width,
                                       config.height, encoderCount));
        }
    }

    std::array<xvr::IEncodedFrameSink*, 2> sinks{};
    sinks[0] = writeFiles ? static_cast<xvr::IEncodedFrameSink*>(&leftFile) : &leftNull;
    sinks[1] = writeFiles ? static_cast<xvr::IEncodedFrameSink*>(&rightFile) : &rightNull;

    xvr::StereoEncodePipeline pipeline;
    pipeline.Initialize(device, context, config, &renderer, sinks, encoderCount);

    // Warm-up frames are excluded: the first encodes include one-off driver allocation
    // and would drag the average down for no reason that reflects steady state.
    for (uint64_t i = 0; i < kWarmupFrames; ++i)
    {
        pipeline.RenderAndEncodeFrame(i, static_cast<double>(i) / config.frameRate);
    }

    // Warm-up submitted flat out, which leaves frames queued inside the encoder. Draining
    // first and then discarding the samples means the measurement starts from an idle
    // encoder instead of charging the first paced frames for that backlog.
    if (paced)
    {
        pipeline.Drain();
        pipeline.ResetLatencyStats();
    }

    const auto start = std::chrono::steady_clock::now();
    const auto frameInterval =
        std::chrono::duration<double>(1.0 / static_cast<double>(config.frameRate));

    for (uint64_t i = 0; i < kMeasuredFrames; ++i)
    {
        const uint64_t index = kWarmupFrames + i;

        // Paced runs submit at the real target rate. Latency measured while submitting
        // flat out is mostly queueing delay, which says nothing about how long the
        // encoder actually takes on a frame it was ready to receive.
        if (paced)
        {
            const auto due = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                         frameInterval * static_cast<double>(i));
            while (std::chrono::steady_clock::now() < due)
            {
                std::this_thread::yield();
            }
        }

        pipeline.RenderAndEncodeFrame(index, static_cast<double>(index) / config.frameRate);
    }

    pipeline.Drain();
    const auto end = std::chrono::steady_clock::now();

    result.wallSeconds = std::chrono::duration<double>(end - start).count();
    result.achievedFps = result.wallSeconds > 0.0 ? kMeasuredFrames / result.wallSeconds : 0.0;

    for (size_t i = 0; i < pipeline.EncoderCount(); ++i)
    {
        const auto& stats = pipeline.Encoder(i).GetStats();
        result.framesSubmitted += stats.framesSubmitted;
        result.framesEncoded += stats.framesEncoded;
        result.bytesEncoded += stats.bytesEncoded;

        const auto latency = pipeline.Encoder(i).GetLatencyStats();
        if (latency.samples > 0 && latency.avgMs > result.latencyAvgMs)
        {
            // Report the worst eye: a stereo frame is only ready when both are.
            result.latencyMinMs = latency.minMs;
            result.latencyAvgMs = latency.avgMs;
            result.latencyMaxMs = latency.maxMs;
        }
    }

    result.sustainedTarget = result.achievedFps >= config.frameRate * 0.95;

    Add(report, std::format(L"  {} encoder(s): {:.1f} fps sustained ({} frames in {:.2f}s) - {}",
                            encoderCount, result.achievedFps, kMeasuredFrames, result.wallSeconds,
                            result.sustainedTarget ? L"MEETS TARGET" : L"BELOW TARGET"));
    Add(report, std::format(L"      encoded {} frames, {:.1f} Mbit total, {:.1f} Mbps per encoder",
                            result.framesEncoded, result.bytesEncoded * 8.0 / 1'000'000.0,
                            result.wallSeconds > 0.0
                                ? (result.bytesEncoded * 8.0 / 1'000'000.0) /
                                      result.wallSeconds / encoderCount
                                : 0.0));

    pipeline.Shutdown();
    leftFile.Close();
    rightFile.Close();

    return result;
}

} // namespace

TestReport Run(ID3D11Device* device, ID3D11DeviceContext* context,
               const std::wstring& outputDirectory)
{
    TestReport report;

    Add(report, L"Stereo render -> NV12 -> H.264 encode test");
    Add(report, L"===========================================");

    // Two candidates: the configuration that matches the Quest's 72Hz exactly, and the
    // largest legal frame, which has to declare 60fps to satisfy MaxMBPS even though the
    // hardware can deliver frames faster than that.
    const xvr::FrameConfig configs[] = {
        xvr::kDefaultPerEyeConfig,
        xvr::kMaxResolutionPerEyeConfig,
    };

    const xvr::FrameConfig& config = configs[0];

    for (const auto& candidate : configs)
    {
        std::wstring reason;
        if (!candidate.Validate(reason))
        {
            Add(report, std::format(L"Configuration {}x{} rejected: {}", candidate.width,
                                    candidate.height, reason));
            continue;
        }

        Add(report, std::format(L"=== {}x{} @{}fps declared  ({} MB/frame, {} MB/s declared) ===",
                                candidate.width, candidate.height, candidate.frameRate,
                                candidate.MacroblocksPerFrame(), candidate.MacroblocksPerSecond()));

        for (const uint32_t encoderCount : { 1u, 2u })
        {
            Add(report, std::format(L"-- {} encoder instance(s) --", encoderCount));
            try
            {
                RunResult result = RunOne(device, context, candidate, encoderCount,
                                          outputDirectory, report);
                if (candidate.width == config.width && candidate.height == config.height)
                {
                    report.runs.push_back(result);
                }
                report.allRuns.push_back(result);
            }
            catch (const xvr::HresultException& e)
            {
                Add(report, std::format(L"  run failed: 0x{:08X}",
                                        static_cast<uint32_t>(e.Code())));
            }
            catch (...)
            {
                Add(report, L"  run failed with an unknown exception");
            }
        }
        Add(report, L"");
    }

    // The decisive comparison is AGGREGATE throughput, not per-instance frame rate.
    //
    // Two encoders each running a little slower than one is not evidence of a shared
    // budget: what matters is whether total work done goes up. A genuinely shared budget
    // means aggregate stays flat (each instance halves). Scaling near 2x means the
    // instances are largely independent, and anything between is contention.
    if (report.runs.size() == 2)
    {
        const double single = report.runs[0].achievedFps;
        const double dual = report.runs[1].achievedFps;
        const double perFrame = static_cast<double>(config.MacroblocksPerFrame());

        const double singleAggregate = single * perFrame;
        const double dualAggregate = dual * perFrame * 2.0;
        const double scaling = singleAggregate > 0.0 ? dualAggregate / singleAggregate : 0.0;

        Add(report, std::format(L"Per encoder:  1x {:.1f} fps    2x {:.1f} fps each", single, dual));
        Add(report, std::format(L"Aggregate:    1x {:.0f} MB/s    2x {:.0f} MB/s   scaling {:.2f}x",
                                singleAggregate, dualAggregate, scaling));
        Add(report, std::format(L"Headroom at {}fps target: {:.2f}x per eye with two encoders",
                                config.frameRate, dual / config.frameRate));

        if (scaling >= 1.6)
        {
            Add(report, L"VERDICT: encoders scale - two instances give ~2x total throughput.");
            Add(report, L"         One encoder per eye is the right architecture.");
            report.summary.push_back(L"Encoders SCALE - one per eye");
        }
        else if (scaling >= 1.2)
        {
            Add(report, L"VERDICT: partial scaling - real contention, but two still beat one.");
            report.summary.push_back(L"Partial scaling");
        }
        else
        {
            Add(report, L"VERDICT: no scaling - the budget is shared. A single side-by-side");
            Add(report, L"         frame gives the same per-eye result more simply.");
            report.summary.push_back(L"Budget SHARED - use side-by-side");
        }

        report.summary.push_back(std::format(L"{}x{}/eye, {:.0f}fps sustained", config.width,
                                             config.height, dual));
    }

    // Encoder latency, measured while submitting at the real target rate. This is a
    // direct component of render-to-photon time, and the reason B-frames and rate-control
    // buffering were turned off above.
    Add(report, L"=== Encoder latency (paced at target rate, 2 encoders) ===");
    for (const auto& candidate : configs)
    {
        std::wstring reason;
        if (!candidate.Validate(reason))
        {
            continue;
        }

        try
        {
            const RunResult paced =
                RunOne(device, context, candidate, 2, std::wstring(), report, true);

            Add(report, std::format(L"  {}x{} @{}fps: min {:.2f}ms  avg {:.2f}ms  max {:.2f}ms",
                                    candidate.width, candidate.height, candidate.frameRate,
                                    paced.latencyMinMs, paced.latencyAvgMs, paced.latencyMaxMs));

            if (candidate.width == config.width)
            {
                report.summary.push_back(std::format(L"Encode latency {:.1f}ms avg",
                                                     paced.latencyAvgMs));
            }
        }
        catch (...)
        {
            Add(report, L"  paced run failed");
        }
    }

    Add(report, L"--- test finished ---");
    return report;
}

} // namespace sample
