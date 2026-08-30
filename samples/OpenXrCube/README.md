# OpenXR cube

A plain OpenXR app. Ring of spinning cubes, a grey cube per hand that brightens with the
trigger. Nothing to write home about visually — that's not what it's for.

The point is what's *not* in [App.cpp](App.cpp): no XVR headers, no framework types, no
awareness that the display is a Quest over Wi-Fi. It's written exactly as it would be for a
desktop headset. The only thing making it stream is the link line, which pulls in
`framework/src/openxr` instead of `openxr_loader`.

If you have an OpenXR app already, this is the whole porting story.

## Building

```
msbuild samples/OpenXrCube/OpenXrCube.vcxproj /p:Configuration=Release /p:Platform=x64 ^
        /p:PackageCertificateThumbprint=<your dev cert>
```

Deploy the `.msix` through Device Portal at `https://<xbox-ip>:11443`. Start the Quest
client and they find each other.

## Careful

Don't add `openxr_loader` to the link line as well. Both export `xrCreateInstance` and you
get a duplicate symbol error — which is the good outcome, because the alternative is one of
them silently winning and the runtime never being used.
