# XVR

Stream VR from an Xbox to a Quest. Like PCVR, except the thing doing the rendering is a
console in Dev Mode.

The Xbox renders stereo, encodes it on the hardware H.264 encoder and sends it over UDP.
The Quest decodes it, reprojects it against your current head pose, and sends head and
controller tracking back every frame. Both ends find each other on their own — no IP
addresses to type.

Working on real hardware: Xbox Series S → Quest 3, 1808x1008 per eye at 72fps.
[docs/xbox-encoder-constraints.md](docs/xbox-encoder-constraints.md) has the measured
latency figures and why the resolution is that particular number.

| <img src="https://lh3.googleusercontent.com/d/1tLH8AGs4CrHS8X46Sv-6fJFYjXL2j9RH" width="80" alt="real rating"> | **Note:** This project was made with the use of AI. Read more about the rating [here](https://www.realgoodai.org/real-rating). |
| :---: | :--- |

## Status

Prototype. It works and it works well, but it's been tested on exactly one console and one
headset, the API isn't stable yet, and half of it was discovered by finding out what the
Xbox refuses to do. Don't build anything load-bearing on the interfaces yet.

## What's here

```
framework/     the reusable part - render targets, NV12, encoder, transport, pose,
               and framework/src/openxr = the OpenXR runtime
samples/       StereoEncode = callback API, OpenXrCube = same thing via OpenXR,
               XrShootingRange = a small game, and the one that proves the runtime works
client/android app/ = flat viewer for a phone, vr/ = the actual Quest client
tools/         device probe, a PC receiver, and the OpenXR test harness
docs/          protocol spec + everything measured about the Xbox encoder
```

You implement `IStereoRenderer::RenderEye` and the framework runs the loop, owns the
encoder, paces frames, handles the network and hands you the headset's pose. It calls you,
not the other way round.

## It's also an OpenXR runtime

An app that already speaks OpenXR links against this instead of the Khronos loader and
streams to a Quest with no source changes — `xrLocateViews` returns the Quest's pose,
`xrEndFrame` hands us the eye textures. `samples/OpenXrCube` is a plain OpenXR app that
doesn't include a single XVR header; the only thing making it stream is the link line.

`samples/XrShootingRange` is the one to run if you want to know whether it actually works.
It's a small game — targets appear around you, you point a controller and shoot them. A
spinning cube proves the image arrives; it can't prove the image is *right*, because it
looks fine whether or not the eye projections match or the poses are in the space they
claim. Hitting something proves it. Miss everything and one of those is wrong.

Instance, session, swapchains, the frame loop and the action system are there. D3D11 only,
primary stereo only. See [docs/openxr-runtime.md](docs/openxr-runtime.md) for what's in and
what isn't.

It has never been run against the Khronos conformance suite, so "implements OpenXR" means
"one real application drives it correctly", not "verified against the spec".

**Not** a way to stream existing Xbox games. UWP sandboxing means an app can't capture
another app's output or install a system runtime, so anything streamed has to be built
against this. That's a hard wall, not a todo.

## Building

Xbox side needs VS 2022 Build Tools with the UWP workload and the Windows 10 SDK. Open the
vcxproj or:

```
msbuild samples/StereoEncode/StereoEncode.vcxproj /p:Configuration=Release /p:Platform=x64
```

Deploy the resulting `.msix` through Device Portal at `https://<xbox-ip>:11443`.

Client side is in [client/android](client/android/README.md) — read the two build gotchas
there before you lose an afternoon to them.

## Worth reading

[docs/xbox-encoder-constraints.md](docs/xbox-encoder-constraints.md) is the useful one.
The Xbox's encoder has undocumented limits, two of which crash rather than return an
error, and none of which are written down anywhere else. If you're doing anything with
Media Foundation on Xbox Dev Mode it'll save you time whether or not you care about VR.

[docs/protocol.md](docs/protocol.md) is the wire format, if you want to write your own
client.

## How this was built

Most of the code and documentation here was written with Claude, working against real
hardware in a loop — build, deploy to the console, read what broke, try again.

The measurements are real. Every number in `docs/` came out of running something on a
physical Series S and a Quest 3; none of it is a model's guess at what the hardware probably
does. That distinction is the whole reason those documents are worth anything, so where you
see a figure, something was run to get it.

The bugs are real too, and several of the sharper warnings in the docs are there because the
AI wrote the bug first and found it days later. The one about declaring 60fps and feeding 90
is the best example — it was confidently documented as a clever trick, shipped, and then
spent a week producing corruption that got blamed on Wi-Fi.

Read it the way you'd read any prototype written fast: the parts that touch hardware have
been beaten into shape by failing on it, and the parts that haven't been exercised yet are
exactly as trustworthy as untested code usually is.

## Licence

MIT. See [LICENSE](LICENSE).
