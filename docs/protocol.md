# XVR wire protocol v3

The contract between the Xbox host and the Quest client. Both sides are built separately
so this and `framework/include/xvr/Protocol.h` have to stay in step — and the Java client
duplicates it again in `Protocol.java`, since C++ and Java can't share a header. Bump the
version in all three or packets get silently dropped and it looks like a dead network.
(Learned that the hard way.)

Little-endian throughout. Timestamps in microseconds.

## Ground rules

Latency beats reliability. Nothing is retransmitted, nothing waits for a missing packet.
A lost fragment loses its frame and the stream carries on. By the time a retransmit
arrived the frame would be two frames stale, so there's no point.

Loss is recovered with keyframes, not repair.

## Packet types

| | | |
| --- | --- | --- |
| 0x01 | VideoFragment | host → client |
| 0x02 | PoseUpdate | client → host |
| 0x03 | Handshake | client → host |
| 0x04 | HandshakeAck | host → client |
| 0x05 | KeyframeRequest | client → host |
| 0x06 | Heartbeat | both (not implemented yet) |

Every packet starts with 8 bytes:

```
0  4  magic    'XVR1'
4  1  type
5  1  version  currently 3
6  2  reserved
```

Wrong magic or version → dropped without parsing. Stops random LAN broadcasts being
treated as video.

## VideoFragment (0x01)

```
0   8  header (type 0x01)
8   1  eye              0 left, 1 right
9   1  flags            bit0 keyframe, bit1 last fragment
10  2  fragmentIndex
12  2  fragmentCount
14  2  reserved
16  4  frameIndex       shared by both eyes
20  8  captureTimeUs    stamped at render, not send
28  4  renderPoseSequence
32  .. H.264 Annex B payload
```

Header is 32 bytes.

`renderPoseSequence` is which pose this frame was rendered against. The client keeps its
last 256 poses and looks it up on arrival — that's what makes reprojection possible.
Echoing 4 bytes beats sending a whole pose on every fragment.

`captureTimeUs` is stamped at render time so latency measurements cover the whole trip,
not just network transit.

**Fragments are 1200 bytes.** Well under the 1472 that fit an Ethernet MTU — the client's
on Wi-Fi and some links carry less. Never rely on IP fragmentation; one lost packet would
take out the frame with no way to know which piece went missing.

### Reassembly

- Collect per `(frameIndex, eye)`.
- Complete when `fragmentCount` distinct fragments have arrived.
- A fragment for a **newer** frame means the old one is dead — drop it immediately. It
  can't be shown on time and holding it just adds latency.
- Also drop incomplete frames after two frame intervals.
- Discard everything until the first keyframe. Decoders can't start mid-GOP.

## PoseUpdate (0x02)

Sent every client frame. Never batched — a pose is worthless once a newer one exists.

```
0    8   header (type 0x02)
8    8   clientTimeUs
16   4   poseSequence
20   4   flags            bit0: eye block present
24   12  headPosition
36   16  headOrientation
52   44  left eye         position[3], orientation[4], fov[4]
96   44  right eye        same
140  76  left controller
216  76  right controller
```

292 bytes total.

`fov` is the frustum half-angles in radians, left/right/up/down (left and down negative).
Per-eye and **asymmetric** — the outer edge extends further than the inner. A host
assuming a symmetric FOV puts everything in the wrong place.

Head pose alone isn't enough: each eye has its own offset and frustum, and IPD varies per
person and device. Send what the runtime actually reports rather than guessing.

Out-of-order poses are dropped, not reordered. But if nothing's been accepted for 500ms,
take whatever arrives and rebase — otherwise a client restart (sequence back to 1) means
every pose looks stale forever and the link dies permanently.

### ControllerState (76 bytes each)

```
0   4   buttons (bitmask)
4   12  grip position
16  16  grip orientation
32  12  aim position
44  16  aim orientation
60  4   trigger   0..1
64  4   squeeze   0..1
68  8   thumbstick x,y
```

Buttons: primary click/touch, secondary click/touch, menu, thumbstick click/touch, trigger
touch, thumbrest touch, and bit 31 = active.

"Primary/secondary" not A/B/X/Y — same physical button, different label per hand, and
anything tracking that mapping will eventually get it backwards.

Grip and aim are both sent. Grip is where the hand is, aim is where the controller points;
you can't derive one from the other without device-specific geometry.

Analogue stays analogue rather than being thresholded, so this maps straight onto
`xrGetActionStateFloat` when the framework starts pretending to be OpenXR.

## Handshake (0x03) / Ack (0x04)

Client broadcasts, host replies to whatever address it heard from. Nobody types an IP —
which matters because doing that on a console with a controller is miserable.

```
Handshake                    Ack
0  8  header (0x03)          0   8  header (0x04)
8  2  requestedWidth         8   2  width   (actual)
10 2  requestedHeight        10  2  height  (actual)
12 1  requestedFrameRate     12  1  frameRate (actual)
13 1  reserved               13  1  codec (0 = H.264)
14 2  clientProtocolVersion  14  2  reserved
                             16  8  hostTimeUs
```

The client asks for its display refresh rate — most Quests default to 72 but the user may
have picked 90 or 120, and the host has no way to guess. Host clamps to what it can do and
the client must use whatever comes back.

## KeyframeRequest (0x05)

```
0  8  header
8  4  lastGoodFrameIndex (0 if none)
12 4  reserved
```

Host rate-limits these. A keyframe is several times a normal frame, so honouring every
request during a loss burst floods the link and causes more loss.

## Versioning

Bump on any change to an existing field's meaning, size or offset. Appending to the end is
fine as long as older readers ignoring the tail still work.

v2 added `renderPoseSequence` (shifted the payload, breaking). v3 appended controller
state.

Once this is public it's a compatibility contract with other people's code, so changes
after release cost their time too.
