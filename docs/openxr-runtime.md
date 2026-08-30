
## Idea

Implement the `xr*` functions ourselves. An app that already speaks OpenXR links against
us instead of the Khronos loader and streams to a Quest with **no source changes** — just
a different library on the link line. `xrLocateViews` hands back the Quest's pose,
`xrEndFrame` hands us the eye textures, we encode and send them. The app never knows.

```
app (unmodified OpenXR code)
  xrLocateViews()  <- pose from the Quest
  xrEndFrame()     -> eye textures into the encoder
        |
   xvr framework (exports the xr* symbols)
        |
   encode -> UDP -> Quest
```

This is built now. `samples/OpenXrCube` is a plain OpenXR app that doesn't include a single
XVR header; the only thing making it stream is the link line.

## What's implemented

Instance, system, session, the D3D11 binding, reference and action spaces, view config,
swapchains, the frame loop, `xrLocateViews`, and the action system. Enough for a normal app.
Anything else returns `XR_ERROR_FUNCTION_UNSUPPORTED` from `xrGetInstanceProcAddr` rather
than being missing at link time, so you find out where you asked for it.

Worth knowing:

- Only `XR_KHR_D3D11_enable`. No D3D12, no Vulkan.
- One view configuration: primary stereo.
- `LOCAL` and `STAGE` are the same origin. The client sends poses in its own local space and
  we have no floor measurement of our own, so pretending otherwise would invent a height
  the headset disagrees with.
- Recommended swapchain size == max == the encoder's frame size. No point offering a size
  that then has to be letterboxed into it.
- Non-projection layers (quads etc.) are accepted and ignored. There's no compositor here —
  one image per eye goes to the encoder. Failing `xrEndFrame` over a layer the app thinks is
  optional would stop the stream dead.
- No haptics. `xrApplyHapticFeedback` succeeds and does nothing, because an error makes apps
  treat working controllers as broken.

## What works and what doesn't

Link-time substitution: **works**. We export the same symbols, the app links us instead of
the loader. No manifest, no registration, no loader involved.

Redirecting an already-compiled app: **doesn't**. Runtimes are found through a
system-registered manifest and UWP won't let us install one or inject into another
process. Same wall that makes "stream any Xbox game" impossible.

So the app has to be **rebuilt against us, not rewritten**. For a Dolphin port that costs
nothing since we're compiling it anyway.

## Why this beats patching a fork

The obvious way to get an existing PCVR mod running on this is to find where it reads head
pose and where it submits frames, and redirect both by hand.

But those mods already call OpenXR. If we implement OpenXR, there's nothing to find and
nothing to patch — it's a link-time swap instead of a source merge into someone else's
fork, and it doesn't need redoing every time they update upstream.

## Two things that bit

Both found by the harness, both would have been miserable on-device.

**The app's device needs multithread protection.** The encoder runs its own event thread and
touches the device from it, so the immediate context gets used from two threads. Our own
`D3DDevice` turns `SetMultithreadProtected` on; an app that created its own device has no
reason to have. Without it you get an access violation several frames later, nowhere near
the cause. `xrCreateSession` sets it now.

**sRGB swapchains can't be fed to the video processor.** It rejects an sRGB view with a bare
`E_INVALIDARG`. The staging copy drops to the `_UNORM` flavour of the same typeless family —
legal for `CopySubresourceRegion`, accepted by the view, identical bits.

## Constraints

- Link ours **instead of** `openxr_loader`, never both. Two libraries exporting
  `xrCreateInstance` is a duplicate symbol error, which is the intended way to find out.
- We only need what an app actually calls, but that set is app-specific. First failure for
  an unfamiliar app will be `XR_ERROR_FUNCTION_UNSUPPORTED` on something we skipped.
- Entry points are `extern "C"` and must never throw. The framework signals failure by
  throwing, so the two conventions meet at the session boundary — anything thrown becomes an
  `XrResult` with the cause logged.

## Testing

`tools/XrHarness` links the same sources into a desktop console exe and drives a full
session against a fake client speaking the real wire protocol over loopback. It checks the
pose maths, binding resolution, that `xrLocateViews` hands back what the client sent, that
actions resolve to the right controls, and that encoded frames actually leave the socket.

Proves nothing about the Xbox's encoder — that hardware isn't here. But API plumbing gets
fixed in seconds instead of a deploy cycle.

```
msbuild tools/XrHarness/XrHarness.vcxproj /p:Configuration=Release /p:Platform=x64
tools/XrHarness/x64/Release/XrHarness.exe
```

## Doesn't replace the callback API

`IStereoRenderer` stays. It's the simpler thing for someone writing new Xbox content and
it's what the sample uses. OpenXR is for porting existing VR code. Same pipeline
underneath.
