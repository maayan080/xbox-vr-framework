#include "xvr/Constraints.h"

#include <format>

namespace xvr {

bool FrameConfig::Validate(std::wstring& reason) const
{
    if (width == 0 || height == 0 || frameRate == 0)
    {
        reason = L"width, height and frame rate must all be non-zero";
        return false;
    }

    const uint32_t perFrame = MacroblocksPerFrame();
    const uint32_t perSecond = MacroblocksPerSecond();

    // Dimension limits are checked before the macroblock budget, because a frame can sit
    // comfortably inside the budget and still be fatal: exceeding the height crashes the
    // encoder inside SetOutputType instead of returning an error.
    if (height > EncoderLimits::kMaxFrameHeight)
    {
        reason = std::format(
            L"height {} exceeds the {} limit. This does NOT fail cleanly - it crashes the "
            L"encoder (1216 and 1344 both crashed on Series S). Total pixels are irrelevant "
            L"here: {}x{} is only {} macroblocks and still fatal.",
            height, EncoderLimits::kMaxFrameHeight, width, height, perFrame);
        return false;
    }

    if (width > EncoderLimits::kMaxFrameWidth)
    {
        reason = std::format(L"width {} exceeds the {} limit", width,
                             EncoderLimits::kMaxFrameWidth);
        return false;
    }

    // Checked first and separately: this is the case that crashes rather than fails.
    if (perFrame > EncoderLimits::kMaxMacroblocksPerFrame)
    {
        reason = std::format(
            L"{}x{} is {} macroblocks/frame, over the {} limit. Note that exactly 8192 "
            L"(e.g. 2048x1024) does not fail cleanly - it crashes the encoder.",
            width, height, perFrame, EncoderLimits::kMaxMacroblocksPerFrame);
        return false;
    }

    if (perSecond > EncoderLimits::kMaxMacroblocksPerSecond)
    {
        const uint32_t maxFps = EncoderLimits::kMaxMacroblocksPerSecond / perFrame;
        reason = std::format(
            L"{}x{}@{} needs {} MB/s, over the {} budget. At this size the maximum is "
            L"{}fps; at this frame rate the maximum is {} macroblocks/frame.",
            width, height, frameRate, perSecond, EncoderLimits::kMaxMacroblocksPerSecond,
            maxFps, EncoderLimits::kMaxMacroblocksPerSecond / frameRate);
        return false;
    }

    if (bitrateBps > EncoderLimits::kMaxBitrateBps)
    {
        reason = std::format(L"{} bps is over the {} bps limit", bitrateBps,
                             EncoderLimits::kMaxBitrateBps);
        return false;
    }

    reason.clear();
    return true;
}

} // namespace xvr
