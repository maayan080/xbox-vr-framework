#include "xvr/EncodedFrameSink.h"

#include "xvr/Log.h"

namespace xvr {

void FileFrameSink::Open(const std::wstring& path)
{
    m_file.open(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!m_file.is_open())
    {
        LogError(L"Could not open output file: {}", path);
        return;
    }
    LogInfo(L"Writing H.264 elementary stream to {}", path);
}

void FileFrameSink::Close()
{
    if (m_file.is_open())
    {
        m_file.flush();
        m_file.close();
        LogInfo(L"Wrote {} frames ({} keyframes), {} bytes", m_frames, m_keyframes, m_bytes);
    }
}

void FileFrameSink::OnEncodedFrame(const EncodedFrame& frame)
{
    if (m_file.is_open() && frame.data && frame.size > 0)
    {
        m_file.write(reinterpret_cast<const char*>(frame.data),
                     static_cast<std::streamsize>(frame.size));
    }

    ++m_frames;
    m_bytes += frame.size;
    if (frame.keyframe)
    {
        ++m_keyframes;
    }
}

} // namespace xvr
