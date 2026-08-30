#pragma once

// Hardware encoder limits, measured on Xbox Series S Dev Mode.
// See docs/xbox-encoder-constraints.md for the measurements behind every number.
//
// These are enforced in code rather than left as documentation because one of them
// (the exact 8192-macroblock frame) crashes the encoder outright instead of returning
// an error, and because exceeding the throughput budget produces a bare
// MF_E_INVALIDMEDIATYPE that says nothing about which limit was hit.

#include <cstdint>
#include <string>

namespace xvr {

struct EncoderLimits
{
    // H.264 Level 4.2 MaxMBPS. Confirmed: 1808x1008@72 (512,568) passes,
    // 1824x1024@72 (525,312) fails, and this value predicts all measured results.
    static constexpr uint32_t kMaxMacroblocksPerSecond = 522'240;

    // 2048x1024 is exactly 8192 macroblocks and HARD-CRASHES the encoder: it passes
    // media type validation, then faults inside the driver. Stay strictly below.
    static constexpr uint32_t kMaxMacroblocksPerFrame = 8191;

    // 40 Mbps accepted, 80 Mbps rejected.
    static constexpr uint32_t kMaxBitrateBps = 40'000'000;

    // Individual dimension limits, separate from the macroblock budget.
    //
    // Exceeding the height CRASHES the encoder inside SetOutputType rather than
    // returning an error: 1024 high works, 1216 and 1344 both crashed on Series S.
    // 1088 is the highest confirmed working value (1920x1088). A frame can be well
    // inside the macroblock budget and still be fatal if it is too tall.
    static constexpr uint32_t kMaxFrameHeight = 1088;

    // 1920 wide is confirmed working; 2048 is cleanly rejected. The true limit lies
    // somewhere between, so the confirmed value is used.
    static constexpr uint32_t kMaxFrameWidth = 1920;

    static constexpr uint32_t kMacroblockSize = 16;
};

// A single encoder instance's frame configuration.
struct FrameConfig
{
    uint32_t width = 1344;
    uint32_t height = 1344;
    uint32_t frameRate = 72;
    uint32_t bitrateBps = 20'000'000;

    // H.264 works in 16x16 macroblocks; partial blocks round up, which is why
    // 1920x1080 and 1920x1088 cost the encoder exactly the same.
    constexpr uint32_t MacroblocksWide() const
    {
        return (width + EncoderLimits::kMacroblockSize - 1) / EncoderLimits::kMacroblockSize;
    }

    constexpr uint32_t MacroblocksHigh() const
    {
        return (height + EncoderLimits::kMacroblockSize - 1) / EncoderLimits::kMacroblockSize;
    }

    constexpr uint32_t MacroblocksPerFrame() const
    {
        return MacroblocksWide() * MacroblocksHigh();
    }

    constexpr uint32_t MacroblocksPerSecond() const
    {
        return MacroblocksPerFrame() * frameRate;
    }

    // Returns false and fills `reason` if this configuration would be rejected by the
    // hardware - or, worse, accepted and then crash it.
    bool Validate(std::wstring& reason) const;
};

// The default target: 1808x1008 @72fps, measured working on real hardware.
//
// A squarer frame would suit a VR eye better at the same encoder cost, but the 1088
// height limit rules that out - 1344x1344 is within the macroblock budget and still
// crashes the encoder. Height is the binding constraint, not total pixels.
// 12 Mbps per eye, not 20. Per eye reads as reasonable; on the wire it is doubled, and
// what the link can carry - not what the encoder will accept - is the binding constraint.
// AdaptBitrate moves it from here based on what the client reports.
inline constexpr FrameConfig kDefaultPerEyeConfig{ 1808, 1008, 72, 12'000'000 };

// The tallest square frame that fits under the height limit. Fewer pixels, but a much
// better match to a headset's field of view if horizontal detail proves less valuable.
inline constexpr FrameConfig kSquarePerEyeConfig{ 1088, 1088, 72, 20'000'000 };

// The largest legal frame: 1920 is the confirmed width limit, 1088 the height limit, and
// 8,160 macroblocks sits just under the 8,192 that crashes the encoder.
//
// Declared at 60fps because MaxMBPS caps the *declared* rate (8,160 x 72 would exceed
// it), but measured throughput is far higher than that limit implies - so frames can
// still be delivered at 72fps. The declared rate governs rate control, not how fast the
// encoder will accept input.
// 12 Mbps per eye, not 25.
//
// The resolution here is the largest frame the Xbox encoder accepts, and the bitrate used
// to be picked to match it - on the encoder's behalf, without asking what the link between
// the console and the headset could carry. Per eye it read as reasonable; on the wire it
// was fifty megabits a second of low-latency UDP, which is more than most home Wi-Fi will
// pass without dropping some of it. A dropped fragment costs an entire frame, so the
// failure did not look like too much bitrate - it looked like torn and smeared pictures
// whenever the scene moved enough to make frames large.
//
// This is only the starting point. AdaptBitrate walks it down when the client reports loss
// and back up when it stops, so the link sets the real number rather than this constant.
inline constexpr FrameConfig kMaxResolutionPerEyeConfig{ 1920, 1088, 60, 12'000'000 };

// Bounds for the adaptive controller.
inline constexpr uint32_t kAdaptiveMinBitrateBps = 4'000'000;
inline constexpr uint32_t kAdaptiveMaxBitrateBps = 25'000'000;

// Fallback if the throughput budget turns out to be shared between instances, in which
// case each eye gets half of it.
inline constexpr FrameConfig kSharedBudgetPerEyeConfig{ 1280, 720, 72, 15'000'000 };

} // namespace xvr
