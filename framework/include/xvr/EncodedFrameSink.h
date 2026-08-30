#pragma once

#include <cstdint>
#include <fstream>
#include <string>

namespace xvr {

struct EncodedFrame
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    int64_t timestampHns = 0;
    bool keyframe = false;

    // Carried through to the wire so the client can pair the two eyes and measure true
    // render-to-photon latency. captureTimeUs is stamped when the frame was rendered,
    // not when it was encoded or sent.
    uint32_t frameIndex = 0;
    uint64_t captureTimeUs = 0;
    uint32_t poseSequence = 0;
    uint8_t eye = 0;
};

// Where encoded frames go. The file sink proves the pipeline works; the network sink
// that replaces it in Phase 1 step 2 implements the same interface, so nothing above
// this line has to change when streaming arrives.
class IEncodedFrameSink
{
public:
    virtual ~IEncodedFrameSink() = default;
    virtual void OnEncodedFrame(const EncodedFrame& frame) = 0;
};

// Writes a raw H.264 Annex B elementary stream, playable directly by ffplay or VLC.
class FileFrameSink : public IEncodedFrameSink
{
public:
    void Open(const std::wstring& path);
    void Close();

    void OnEncodedFrame(const EncodedFrame& frame) override;

    uint64_t FrameCount() const { return m_frames; }
    uint64_t BytesWritten() const { return m_bytes; }
    uint64_t KeyframeCount() const { return m_keyframes; }

private:
    std::ofstream m_file;
    uint64_t m_frames = 0;
    uint64_t m_bytes = 0;
    uint64_t m_keyframes = 0;
};

// Counts and discards. Used for throughput measurement, where writing to storage would
// measure the filesystem rather than the encoder.
class NullFrameSink : public IEncodedFrameSink
{
public:
    void OnEncodedFrame(const EncodedFrame& frame) override
    {
        ++m_frames;
        m_bytes += frame.size;
        if (frame.keyframe)
        {
            ++m_keyframes;
        }
    }

    uint64_t FrameCount() const { return m_frames; }
    uint64_t BytesWritten() const { return m_bytes; }
    uint64_t KeyframeCount() const { return m_keyframes; }

private:
    uint64_t m_frames = 0;
    uint64_t m_bytes = 0;
    uint64_t m_keyframes = 0;
};

} // namespace xvr
