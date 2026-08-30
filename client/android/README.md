# Android / Quest clients

Two apps sharing one copy of the network and decode code (`shared/java`):

- **`app`** — flat 2D viewer. Runs on any Android phone, shows both eyes side by side with
  stats. Built first so decode and networking could be proven without OpenXR in the way,
  and still the fastest way to check whether a problem is the stream or the VR app.
- **`vr`** — the real client. OpenXR, reprojection, controllers.

## Building

```
export JAVA_HOME="C:\Program Files\Android\Android Studio\jbr"
cd client/android
gradle assembleRelease
```

APKs land in `app/build/outputs/apk/release/` and `vr/build/outputs/apk/release/`.

Two things that will waste your afternoon:

**JDK 26 doesn't work.** Gradle 8.x rejects it with "Unsupported class file major version
70". Android Studio's bundled JDK 21 is fine, hence the `JAVA_HOME` above.

**`local.properties` needs forward slashes.** It's a Java properties file so backslash is
an escape character — `C:\Users\...` makes `\U` an invalid escape and the SDK path parses
as garbage. The error is `java.io.IOException: Invalid file path`, which looks nothing
like a quoting problem.

```
sdk.dir=C:/Users/you/AppData/Local/Android/Sdk
```

## Installing

```
adb install -r XvrVR.apk
```

Quest needs developer mode on. Shows up under Unknown Sources.

## Running

Start the Xbox app, launch this on the same Wi-Fi. No addresses to type — the client
broadcasts and the host replies to whoever asked. Either side can be restarted in any
order.

While it's searching you get a coloured screen: red = nothing arriving, amber = packets but
no keyframe yet, blue = decoding but nothing on screen. Saves digging through logcat to
find out how far it got.

## Notes

- Decode runs on the receive thread and renders straight to a Surface. No GPU readback, no
  hop to the UI thread.
- MediaCodec in async mode — polling for output only when new data arrives leaves each
  decoded frame sitting there waiting for the next one. That was worth ~12ms.
- Frames before the first keyframe get dropped. Decoders can't start mid-GOP.
- When the decoder falls behind we replace the pending frame instead of queueing. A
  backlog of stale frames is what makes a stream feel late.
- Each eye latches independently and reprojects with its own pose. An earlier version held
  both until each had a new frame — that cost a third of the frame rate and showed up as
  micro-stutter.

## Useful

```
adb logcat -s XvrVrActivity:V
```

Prints motion-to-photon latency and a per-second breakdown of arrived / decoded / skipped
frames, which is how you tell a network problem from a decoder problem from a display
problem.
