# Xbox Dev Mode encoder limits

All measured on an actual Xbox Series S (`XBOX_SCARLETT_LO`, OS 10.0.26100.8866) in Dev
Mode, not read off a spec sheet. Most of this isn't documented anywhere, which is why
it's written down.

## Summary

| | |
| --- | --- |
| App memory budget | 5120 MiB |
| GPU dedicated / shared | 128 MiB / 1280 MiB |
| H.264 encoder | `H.264 HW Encoder MFT`, works |
| HEVC encoder | present, rejected every config we tried |
| D3D12 Video Encode | **not exposed** — Media Foundation is the only option |
| Max frame | **< 8192 macroblocks**, and width ≤ 1920, height ≤ 1088 |
| Max throughput | 522,240 macroblocks/sec (H.264 Level 4.2 MaxMBPS) |
| Max bitrate | somewhere between 40 and 80 Mbps |
| Concurrent encoders | 4 configure fine; 2 is what we use |

## Two things that crash rather than fail

**Height above 1088.** Crashes inside `SetOutputType`. Not an error return — an access
violation you can't catch.

| Frame | MB | Result |
| --- | --- | --- |
| 1920x1088 | 8,160 | fine |
| 1024x1024 | 4,096 | fine |
| 1216x1216 | 5,776 | **crash** |
| 1344x1344 | 7,056 | **crash** |
| 2560x1440 | 14,400 | clean rejection |

Note 1344x1344 is well inside every budget and still kills it. Being under the macroblock
limit isn't enough. Also: no square frames, which is annoying because square suits a VR eye
better. 1088x1088 is the tallest you get.

**2048x1024 exactly.** That's exactly 2^21 samples / 8192 MB. Passes validation, then
crashes. 2048x1088 rejects cleanly, so it's not the width — it's the exact boundary.

## Resolution vs frame rate

60fps was never a cap, it's just what 1080p costs. Everything that passes is under
522,240 MB/s:

| Config | MB/s | |
| --- | --- | --- |
| 1280x720 @144 | 518,400 | ok |
| 1600x900 @90 | 513,000 | ok |
| 1808x1008 @72 | 512,568 | ok — highest we found |
| 1824x1024 @72 | 525,312 | fails |
| 1920x1088 @72 | 587,520 | fails |

At 72fps that's 7,253 MB/frame.

**MaxMBPS only limits what you can *declare*.** The hardware is much faster — one encoder
sustained ~1,333,000 MB/s, about 2.5x the declared limit. Two encoders, one per eye,
sustained 141.6 fps each at 1920x1088.

An earlier version of this document took that to mean you could declare 60fps for a legal
1920x1088 frame and then feed it at 90, and get max resolution at a high frame rate for
free. **Don't. It cost us a day.**

The frames do come out, which is what makes it look like it works. But rate control budgets
bits *per frame* on the assumption that the declared number of them makes a second. Feed 90
into an encoder told 60 and every frame still gets a sixtieth of a second's worth of bits,
so you emit **half again the bitrate you asked for** — 25 Mbps per eye becomes 37.5, and two
eyes put 75 Mbps on the air where you intended 50. Sample timestamps are wrong by the same
ratio, since each is stamped as lasting 1/60 s while arriving every 1/90.

Over Wi-Fi that surfaces as torn and smeared pictures whenever the scene moves enough to
make frames large — which reads as packet loss or a weak link, not as a bitrate you
misconfigured. We halved the configured bitrate twice while the real figure stayed 1.5x
whatever we set, and neither change did what it should have.

Declare the rate you intend to feed. If you want more frames, buy them with pixels: at 72fps
the budget is 7,253 MB/frame, and 1808x1008 is the largest we measured passing.

## Codec controls

`AVLowLatencyMode` is rejected on Xbox. Doesn't matter much — `BPictureCount = 0` works,
and that's the main thing the preset would have done. B-frames are the real latency
killer.

Works: `BPictureCount`, `RateControlMode`, `MeanBitRate`, `BufferSize`, `QualityVsSpeed`,
`GOPSize`.
Rejected: `AVLowLatencyMode`, `MaxNumRefFrame`, slice control, CABAC.

No slice control means no sub-frame output, so the trick ALVR uses to shave latency isn't
available. Frame granularity is the floor.

**Don't trust `IsSupported` / `IsModifiable`.** Xbox says `modifiable=true` for things
that then fail, and `supported=false` for `BPictureCount` which works. A PC running the
same code said `modifiable=false` for everything including things that set fine. Only the
return value of `SetValue` means anything.

## Latency

Motion-to-photon, timed on the client by clocking a pose's round trip (no clock sync
needed since both ends are the same device).

These were measured at 1920x1088/eye fed at 90fps into a 90Hz Quest — i.e. using the
overdriven configuration described above, before we understood it was overdriven. The
absolute numbers therefore describe a setup we no longer ship, and were taken at roughly
1.5x the intended bitrate. The *relative* findings still hold, and they are the useful part:

| | floor | avg |
| --- | --- | --- |
| baseline | 44 ms | 82 ms |
| + encoder output on its own thread | 44 ms | 80 ms |
| + async MediaCodec on the client | 33 ms | 68 ms |
| + stopped gating both eyes together | 33 ms | ~70 ms, but 90 fps actually displayed |

Encoder fix was worth ~2ms, decoder fix ~12ms. Predicted the opposite. Measure one change
at a time.

Worth restating, since it is the same lesson twice: both the latency work and the frame rate
bug above came down to a number that was assumed rather than checked. The encoder reports
what it was told, not what it is doing.

The gap between floor and average is frame-rate mismatch, not queueing — video landing at
a different phase each display frame. Matching rates fixes it; removing more queues
doesn't.

## Gotchas for anyone building on this

- Set the **output** media type before the input type.
- Encoder is D3D11-aware; send `MFT_MESSAGE_SET_D3D_MANAGER` before setting types.
- Xbox rejects `D3D11_BIND_VIDEO_ENCODER` on NV12 (`E_INVALIDARG`). PC accepts it. Use
  `RENDER_TARGET` only.
- Give every capability test a **freshly enumerated `IMFActivate`**. Reuse one and a
  single failure cascades into fake failures for everything after it.
- Don't assume encoder index 0 works. Enumerate and take the first that actually
  activates — a machine can advertise an encoder with no hardware behind it.
- Winsock works fine in UWP on Xbox. No need for `Windows.Networking.Sockets`.
- Never `Present()` with vsync in a streaming host. Our debug overlay did, and silently
  capped the whole stream at the TV's refresh rate.
