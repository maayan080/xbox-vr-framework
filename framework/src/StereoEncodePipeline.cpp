#include "xvr/StereoEncodePipeline.h"

#include "xvr/Log.h"

#include <chrono>

namespace xvr {
namespace {
constexpr int64_t kHnsPerSecond = 10'000'000;
constexpr float kClearColour[4] = { 0.04f, 0.05f, 0.08f, 1.0f };
} // namespace

void StereoEncodePipeline::Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                                      const FrameConfig& perEye, IStereoRenderer* renderer,
                                      const std::array<IEncodedFrameSink*, 2>& sinks,
                                      uint32_t encoderCount)
{
    m_context = context;
    m_renderer = renderer;
    m_config = perEye;
    m_frameDurationHns = kHnsPerSecond / static_cast<int64_t>(perEye.frameRate);

    for (size_t eye = 0; eye < m_targets.size(); ++eye)
    {
        m_targets[eye].Initialize(device, perEye.width, perEye.height);
        m_converters[eye].Initialize(device, context, perEye.width, perEye.height);
    }

    m_renderer->Initialize(device, context);

    for (uint32_t i = 0; i < encoderCount; ++i)
    {
        auto encoder = std::make_unique<VideoEncoder>();
        encoder->SetEye(static_cast<uint8_t>(i));
        encoder->Initialize(device, perEye, sinks[i]);
        m_encoders.push_back(std::move(encoder));
    }

    LogInfo(L"Pipeline ready: {}x{} per eye @{}fps, {} encoder instance(s)", perEye.width,
            perEye.height, perEye.frameRate, encoderCount);
}

void StereoEncodePipeline::RenderAndEncodeFrame(uint64_t frameIndex, double timeSeconds)
{
    FrameContext frame;
    frame.frameIndex = frameIndex;
    frame.timeSeconds = timeSeconds;
    RenderAndEncodeFrame(frame);
}

void StereoEncodePipeline::RenderAndEncodeFrame(const FrameContext& frame)
{
    const uint64_t frameIndex = frame.frameIndex;

    // Both eyes are always rendered, even when only one encoder is running, so that the
    // GPU cost is identical across configurations and the measurement isolates the
    // encoder rather than the renderer.
    for (size_t eye = 0; eye < m_targets.size(); ++eye)
    {
        m_targets[eye].Clear(m_context, kClearColour);
        m_renderer->RenderEye(frame, static_cast<Eye>(eye), m_targets[eye].View());
    }

    const int64_t timestamp = static_cast<int64_t>(frameIndex) * m_frameDurationHns;

    // Stamped here, after rendering and before encoding, so it measures the age of the
    // image rather than the age of the packet.
    const auto captureTimeUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    for (size_t i = 0; i < m_encoders.size(); ++i)
    {
        ID3D11Texture2D* nv12 = m_converters[i].Convert(m_targets[i].Texture());
        m_encoders[i]->Submit(nv12, timestamp, static_cast<uint32_t>(frameIndex), captureTimeUs,
                              frame.poseSequence);
    }
}

void StereoEncodePipeline::ResetLatencyStats()
{
    for (auto& encoder : m_encoders)
    {
        encoder->ResetLatencyStats();
    }
}

void StereoEncodePipeline::Drain()
{
    for (auto& encoder : m_encoders)
    {
        encoder->Drain();
    }
}

void StereoEncodePipeline::Shutdown()
{
    for (auto& encoder : m_encoders)
    {
        encoder->Shutdown();
    }
    m_encoders.clear();
}

} // namespace xvr
