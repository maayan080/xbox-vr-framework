# Device probe

Throwaway tool that answers the questions a PC can't: what the Xbox's encoder will
actually accept, what the real memory budget is, and whether any of this was going to work
at all.

Everything it found is written up in [docs/xbox-encoder-constraints.md](../../docs/xbox-encoder-constraints.md).
You probably want that instead of running this — it's here because it's how those numbers
were obtained, and it'd be useful again on different hardware.

## What it reports

- Console model, OS build, real `AppMemoryUsageLimit`
- GPU adapter, feature level, video memory
- Display modes the headset/TV advertises
- Whether D3D12 Video Encode exists (it doesn't, on Series S)
- Every H.264 and HEVC encoder MFT, then actually tries to configure the hardware one at a
  range of resolutions, frame rates, bitrates and profiles
- How many encoders run concurrently

Each configuration gets a **freshly enumerated `IMFActivate`**. Reuse one and a single
failure cascades into fake failures for everything after it, which is exactly what
happened the first time and produced a completely wrong picture of the hardware.

## Running it

Deploy via Device Portal at `https://<xbox-ip>:11443` → My games & apps → Add. Feed it
`XboxVRDev.cer` if it asks for a certificate.

Headline results render on screen. Full detail goes to
`LocalState/device-probe.log`, which you can pull through Device Portal's file explorer.

## Warning

Don't add 2048x1024 to the test list. It's exactly 8192 macroblocks, passes validation,
then hard-crashes the encoder. Killed a probe run mid-way and cost a round trip figuring
out why the log just stopped.
