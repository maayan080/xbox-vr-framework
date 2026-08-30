# OpenXR shooting range

A small game. Amber targets appear in a dome around you at varying distances; point a
controller and pull the trigger to pop one. Hits flash green and respawn elsewhere. Score
stacks up as blue cubes on a post to your left, ten per column.

## Why a game and not a cube

[OpenXrCube](../OpenXrCube) proves the image arrives. It cannot prove the image is
*correct* — a rotating cube looks fine whether or not the eye projections match, whether or
not the poses are in the space they claim, whether or not the two eyes agree.

Aiming proves it. To hit anything, all of these have to agree in one space at the same time:

- the aim pose the runtime reports for the controller
- its orientation, in the right handedness, unconjugated
- the view pose for each eye
- both asymmetric eye projections
- the reference space the targets live in

Get any one wrong and the maths still runs, the targets still render, and you simply never
hit anything. **A score going up is a claim about correctness a spinning cube cannot make.**

The floor grid is there for the same reason: without something underfoot there is no
parallax reference, and a wrong world scale or interpupillary distance is easy to miss. With
it, the floor is either at your feet or it is not.

## What it exercises

Instance, system, session, the full session state machine, swapchain acquire/wait/release,
the frame loop, `xrLocateViews`, action sets, `aim/pose` action spaces, float actions with
subaction paths, and interaction profile bindings — the surface a real application uses.

## Not in this file

No XVR headers, no framework types, no awareness that the display is a Quest over Wi-Fi. It
is written exactly as it would be for a desktop headset. The only thing making it stream is
the link line, which pulls in `framework/src/openxr` instead of `openxr_loader`.

## Building

```
msbuild samples/XrShootingRange/XrShootingRange.vcxproj /p:Configuration=Release ^
        /p:Platform=x64 /p:PackageCertificateThumbprint=<your dev cert>
```

Deploy the `.msix` through Device Portal at `https://<xbox-ip>:11443`. Start the Quest
client and they find each other.

## Careful

Don't add `openxr_loader` to the link line as well. Both export `xrCreateInstance` and you
get a duplicate symbol error — which is the good outcome, because the alternative is one of
them silently winning and the runtime never being used.

## Aim, not grip

The pose bound here is `/user/hand/*/input/aim/pose`, not `grip`. Grip is where the hand is;
aim is where the controller points. Binding grip makes every shot miss high and left, which
looks convincingly like a tracking bug rather than a wrong binding.
