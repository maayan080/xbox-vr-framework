#pragma once

#include "xvr/Constraints.h"
#include "xvr/EncodedFrameSink.h"
#include "xvr/NV12Converter.h"
#include "xvr/RenderTarget.h"
#include "xvr/StereoRenderer.h"
#include "xvr/VideoEncoder.h"

#include <array>
#include <memory>
#include <vector>

namespace xvr {

// Render both eyes, convert each to NV12, and encode.
//
// `encoderCount` is configurable rather than fixed at two because whether the hardware's
// throughput budget is shared between encoder instances or granted per instance is still
// an open question, and it decides the architecture:
//
//   - per instance: two encoders give full per-eye resolution
//   - shared:       two encoders and one side-by-side frame are equivalent, so the
//                   simpler single-encoder design wins
//
// Running the identical workload at 1 and at 2 encoders is what distinguishes them.
class StereoEncodePipeline
{
public:
    void Initialize(ID3D11Device* device, ID3D11DeviceContext* context, const FrameConfig& perEye,
                    IStereoRenderer* renderer, const std::array<IEncodedFrameSink*, 2>& sinks,
                    uint32_t encoderCount);

    void RenderAndEncodeFrame(uint64_t frameIndex, double timeSeconds);

    /**
     * Renders using a supplied pose rather than a default viewpoint.
     *
     * The pose is applied to the frame about to be rendered, not the previous one: the
     * whole point of the link is that the image is generated for where the head is now.
     */
    void RenderAndEncodeFrame(const FrameContext& frame);

    void Drain();
    void Shutdown();

    // Clears latency history on every encoder. Call after warm-up so the measurement
    // does not include the backlog those frames left behind.
    void ResetLatencyStats();

    size_t EncoderCount() const { return m_encoders.size(); }
    const VideoEncoder& Encoder(size_t index) const { return *m_encoders[index]; }

private:
    ID3D11DeviceContext* m_context = nullptr;
    IStereoRenderer* m_renderer = nullptr;

    std::array<RenderTarget, 2> m_targets;
    std::array<NV12Converter, 2> m_converters;
    std::vector<std::unique_ptr<VideoEncoder>> m_encoders;

    FrameConfig m_config;
    int64_t m_frameDurationHns = 0;
};

} // namespace xvr
