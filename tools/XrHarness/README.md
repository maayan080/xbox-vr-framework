# OpenXR harness

Drives the OpenXR runtime through a full session on the build machine, against a fake client
that speaks the real wire protocol over loopback.

Desktop console exe, not a UWP package — the point is to run it right here. A deploy cycle
to a console is a bad way to find out you got a handle type wrong.

```
msbuild tools/XrHarness/XrHarness.vcxproj /p:Configuration=Release /p:Platform=x64
tools/XrHarness/x64/Release/XrHarness.exe
```

## What it checks

Pose maths (compose, invert, the degenerate-quaternion repair), binding path resolution,
then a real 150-frame session: `xrLocateViews` hands back the pose the client sent, the
asymmetric frustum survives the round trip, actions resolve to the right controls and hands,
`xrLocateSpace` puts the controller where the client said, and encoded frames actually leave
the socket.

That last one matters on its own — "the encoder ran" and "the encoder ran and the result
reached the network" are different failures with identical symptoms.

## What it doesn't check

Anything about the Xbox's encoder. This box has an NVIDIA card and NVENC accepts things
Xbox's block doesn't. Green here says the API plumbing is right, nothing more. See
[docs/xbox-encoder-constraints.md](../../docs/xbox-encoder-constraints.md).

Detail goes to `xr-harness.log` next to the exe.
