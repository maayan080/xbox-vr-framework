#pragma once

// The wire contract between the Xbox host and the Quest client.
//
// This header and docs/protocol.md are the single source of truth. The two applications
// are built separately - eventually in separate repositories - so nothing here may change
// without changing both sides and bumping kProtocolVersion.
//
// All integers are little-endian; all timestamps are microseconds.

#include <cstdint>

namespace xvr::net {

inline constexpr uint32_t kMagic = 0x31525658; // 'XVR1' little-endian

// Version 2 added renderPoseSequence to the video fragment header. That shifted the
// payload offset, so it is a breaking change and both sides must match.
// Version 3 appended controller state to PoseUpdate.
inline constexpr uint8_t kProtocolVersion = 3;

// Payload bytes per video fragment. Deliberately well under the 1472 that fit a 1500-byte
// Ethernet MTU: the client is on Wi-Fi and some links carry less. IP-level fragmentation
// must never be relied on - it would turn one lost packet into a lost frame with no way
// to know which piece went missing.
inline constexpr uint16_t kFragmentPayloadBytes = 1200;

enum class PacketType : uint8_t
{
    VideoFragment = 0x01,
    PoseUpdate = 0x02,
    Handshake = 0x03,
    HandshakeAck = 0x04,
    KeyframeRequest = 0x05,
    Heartbeat = 0x06,
};

enum VideoFlags : uint8_t
{
    VideoFlag_None = 0,
    VideoFlag_Keyframe = 1 << 0,
    VideoFlag_FinalFragment = 1 << 1,
};

#pragma pack(push, 1)

struct CommonHeader
{
    uint32_t magic = kMagic;
    uint8_t type = 0;
    uint8_t version = kProtocolVersion;
    uint16_t reserved = 0;
};
static_assert(sizeof(CommonHeader) == 8, "wire layout changed");

struct VideoFragmentHeader
{
    CommonHeader common;
    uint8_t eye = 0; // 0 = left, 1 = right
    uint8_t flags = 0;
    uint16_t fragmentIndex = 0;
    uint16_t fragmentCount = 0;
    uint16_t reserved = 0;
    uint32_t frameIndex = 0;
    // Stamped at render time, not send time, so the client can measure true
    // render-to-display latency rather than only network transit.
    uint64_t captureTimeUs = 0;
    // Which PoseUpdate this frame was rendered against.
    //
    // The client keeps a short history of the poses it sent and looks this up on arrival,
    // so it knows exactly where the head was when the image was generated. That is what
    // makes reprojection possible: without it the client cannot tell how far the head has
    // moved since, and can only display a stale frame as though it were current.
    //
    // Echoing the sequence costs 4 bytes; sending the whole pose would cost that much on
    // every fragment of every frame.
    uint32_t renderPoseSequence = 0;
};
static_assert(sizeof(VideoFragmentHeader) == 32, "wire layout changed");

enum PoseFlags : uint32_t
{
    PoseFlag_None = 0,
    // Set when the per-eye block below is populated. Older senders leave this clear and
    // the previously-reserved field reads as zero, so the meaning is compatible.
    PoseFlag_HasEyeData = 1 << 0,
};

/**
 * One eye's viewpoint, as the headset reports it.
 *
 * The head pose alone is not enough to render correct stereo: each eye sits at its own
 * offset and looks through its own asymmetric frustum. Sending what the runtime actually
 * reports avoids the host guessing an IPD and a field of view, which would misplace
 * everything and undo the point of having the pose at all.
 */
struct EyeView
{
    float position[3]{};    // metres, relative to the tracking origin
    float orientation[4]{}; // quaternion x, y, z, w
    // Frustum half-angles in radians: left, right, up, down. Left and down are negative.
    float fov[4]{};
};

// Button and touch bits, named after the OpenXR actions they carry.
//
// "Primary" and "secondary" rather than A/B/X/Y: the same physical positions are named
// differently on each hand, and a host that has to remember which is which per hand will
// eventually get it wrong.
// Unsigned shifts throughout: `1 << 31` is a signed int and overflows, which makes the top
// bit -2147483648 rather than 0x80000000. It happened to produce the right bit pattern here,
// but it is undefined behaviour and any build with warnings-as-errors rejects it outright.
enum ControllerButtons : uint32_t
{
    Button_None = 0u,
    Button_PrimaryClick = 1u << 0,   // A on right, X on left
    Button_PrimaryTouch = 1u << 1,
    Button_SecondaryClick = 1u << 2, // B on right, Y on left
    Button_SecondaryTouch = 1u << 3,
    Button_MenuClick = 1u << 4,
    Button_ThumbstickClick = 1u << 5,
    Button_ThumbstickTouch = 1u << 6,
    Button_TriggerTouch = 1u << 7,
    Button_ThumbrestTouch = 1u << 8,
    Button_Active = 1u << 31, // controller is present and tracking
};

/**
 * One controller, modelled directly on OpenXR's input actions.
 *
 * Grip and aim are both carried because they mean different things: grip is where the
 * hand is, aim is where the controller points. A renderer wanting to draw a hand needs
 * the first; anything pointing at something needs the second, and deriving one from the
 * other requires assumptions about controller geometry that vary by device.
 *
 * Analogue values stay as floats rather than being thresholded into booleans, so a host
 * can see partial trigger pulls - and so this maps straight onto xrGetActionStateFloat
 * when the framework presents itself as an OpenXR runtime.
 */
struct ControllerState
{
    uint32_t buttons = 0;
    float gripPosition[3]{};
    float gripOrientation[4]{ 0, 0, 0, 1 };
    float aimPosition[3]{};
    float aimOrientation[4]{ 0, 0, 0, 1 };
    float trigger = 0.0f;   // 0..1
    float squeeze = 0.0f;   // 0..1
    float thumbstick[2]{};  // -1..1 each axis
};

struct PoseUpdate
{
    CommonHeader common;
    uint64_t clientTimeUs = 0;
    uint32_t poseSequence = 0;
    uint32_t flags = 0;
    float headPosition[3]{};    // metres
    float headOrientation[4]{}; // quaternion x, y, z, w
    // Present when PoseFlag_HasEyeData is set.
    EyeView eyes[2]{};
    // Index 0 is left, 1 is right. Check Button_Active before using either.
    ControllerState controllers[2]{};
};
static_assert(sizeof(EyeView) == 44, "wire layout changed");
static_assert(sizeof(ControllerState) == 76, "wire layout changed");
static_assert(sizeof(PoseUpdate) == 292, "wire layout changed");

struct Handshake
{
    CommonHeader common;
    uint16_t requestedWidth = 0;  // per eye
    uint16_t requestedHeight = 0; // per eye
    uint8_t requestedFrameRate = 0;
    uint8_t reserved = 0;
    uint16_t clientProtocolVersion = kProtocolVersion;
};
static_assert(sizeof(Handshake) == 16, "wire layout changed");

struct HandshakeAck
{
    CommonHeader common;
    uint16_t width = 0;  // per eye, actual
    uint16_t height = 0; // per eye, actual
    uint8_t frameRate = 0;
    uint8_t codec = 0; // 0 = H.264
    uint16_t reserved = 0;
    uint64_t hostTimeUs = 0; // for clock offset estimation
};
static_assert(sizeof(HandshakeAck) == 24, "wire layout changed");

struct KeyframeRequest
{
    CommonHeader common;
    uint32_t lastGoodFrameIndex = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(KeyframeRequest) == 16, "wire layout changed");

struct Heartbeat
{
    CommonHeader common;
    uint64_t senderTimeUs = 0;
};
static_assert(sizeof(Heartbeat) == 16, "wire layout changed");

#pragma pack(pop)

// Rejects anything that is not ours before it is parsed further - which is what stops an
// unrelated broadcast on the LAN from being interpreted as video.
inline bool IsValidHeader(const CommonHeader& header)
{
    return header.magic == kMagic && header.version == kProtocolVersion;
}

} // namespace xvr::net
