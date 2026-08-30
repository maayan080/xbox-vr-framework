#pragma once

#include <cstdint>
#include <vector>

#include <d3d11_4.h>

#include <winrt/base.h>

namespace xvr {

// Converts the renderer's BGRA output into the NV12 the hardware encoder requires,
// using the GPU's video processor rather than a shader or a CPU copy.
//
// Hands out textures from a ring buffer: with an asynchronous encoder MFT the previous
// frame may still be being read when the next is submitted, so a single texture would
// be overwritten underneath the encoder.
class NV12Converter
{
public:
    void Initialize(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width,
                    uint32_t height, uint32_t poolSize = 8);

    // Converts `source` and returns the pool texture holding the result. The caller
    // must submit it to the encoder before calling Convert() poolSize more times.
    ID3D11Texture2D* Convert(ID3D11Texture2D* source);

private:
    struct PoolEntry
    {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11VideoProcessorOutputView> outputView;
    };

    winrt::com_ptr<ID3D11VideoDevice> m_videoDevice;
    winrt::com_ptr<ID3D11VideoContext> m_videoContext;
    winrt::com_ptr<ID3D11VideoProcessor> m_processor;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> m_enumerator;

    std::vector<PoolEntry> m_pool;
    size_t m_next = 0;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

} // namespace xvr
