// The viewer page, embedded so the receiver stays a single self-contained .exe.
//
// Decoding happens in the browser via WebCodecs, which is what lets a phone display the
// Xbox's output with nothing installed. Where WebCodecs is unavailable the page still
// reports statistics, and the .h264 files on disk remain the fallback for verification.

extern const char* kViewerHtml;

const char* kViewerHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>XVR Stream</title>
<style>
  :root { color-scheme: dark; }
  body {
    margin: 0; background: #0d0f17; color: #e6e9ef;
    font: 14px/1.5 ui-monospace, Consolas, monospace;
  }
  header { padding: 12px 16px; border-bottom: 1px solid #222738; }
  h1 { margin: 0; font-size: 15px; font-weight: 600; letter-spacing: .04em; }
  #eyes { display: flex; flex-wrap: wrap; gap: 8px; padding: 12px; }
  .eye { flex: 1 1 320px; min-width: 0; }
  .eye h2 { margin: 0 0 6px; font-size: 12px; color: #8b93a7; font-weight: 500; }
  canvas { width: 100%; height: auto; background: #05070c; border-radius: 6px; display: block; }
  #stats { padding: 0 16px 16px; color: #8b93a7; }
  #stats b { color: #66f28c; font-weight: 600; }
  .warn { color: #ffb454; padding: 12px 16px; }
</style>
</head>
<body>
<header><h1>XVR STREAM &mdash; Xbox to browser</h1></header>
<div id="warn" class="warn" hidden></div>
<div id="eyes">
  <div class="eye"><h2>LEFT EYE</h2><canvas id="c0"></canvas></div>
  <div class="eye"><h2>RIGHT EYE</h2><canvas id="c1"></canvas></div>
</div>
<div id="stats">connecting&hellip;</div>

<script>
const stats = document.getElementById('stats');
const warn = document.getElementById('warn');

if (!('VideoDecoder' in window)) {
  warn.hidden = false;
  warn.textContent =
    'This browser has no WebCodecs support, so video cannot be shown here. ' +
    'Statistics below still work, and the receiver is writing playable .h264 files.';
}

const eyes = [0, 1].map(i => {
  const canvas = document.getElementById('c' + i);
  return {
    canvas,
    ctx: canvas.getContext('2d'),
    decoder: null,
    // A decoder cannot start mid-GOP: feeding it delta frames before a keyframe
    // produces errors, not pictures.
    sawKeyframe: false,
    frames: 0
  };
});

function makeDecoder(eye) {
  const decoder = new VideoDecoder({
    output: frame => {
      if (eye.canvas.width !== frame.displayWidth) {
        eye.canvas.width = frame.displayWidth;
        eye.canvas.height = frame.displayHeight;
      }
      eye.ctx.drawImage(frame, 0, 0);
      frame.close();
      eye.frames++;
    },
    error: e => { console.warn('decoder error', e); eye.sawKeyframe = false; }
  });

  // Annex B means the stream carries its own SPS/PPS inline, so no out-of-band
  // description is needed - which is exactly how the encoder is configured to emit.
  decoder.configure({ codec: 'avc1.4D0028', optimizeForLatency: true, hardwareAcceleration: 'prefer-hardware' });
  return decoder;
}

let received = 0, bytes = 0, lastBytes = 0, lastFrames = 0, mbps = 0, fps = 0;

const ws = new WebSocket('ws://' + location.host + '/');
ws.binaryType = 'arraybuffer';

ws.onopen = () => { stats.textContent = 'connected, waiting for video…'; };
ws.onclose = () => { stats.textContent = 'disconnected'; };

ws.onmessage = ev => {
  const bytesIn = new Uint8Array(ev.data);
  const eyeIndex = bytesIn[0];
  const isKeyframe = bytesIn[1] === 1;
  const payload = bytesIn.subarray(2);

  received++;
  bytes += payload.byteLength;

  const eye = eyes[eyeIndex];
  if (!eye || !('VideoDecoder' in window)) return;

  if (!eye.decoder) {
    try { eye.decoder = makeDecoder(eye); }
    catch (e) { console.warn(e); return; }
  }

  if (!eye.sawKeyframe) {
    if (!isKeyframe) return;
    eye.sawKeyframe = true;
  }

  try {
    eye.decoder.decode(new EncodedVideoChunk({
      type: isKeyframe ? 'key' : 'delta',
      timestamp: performance.now() * 1000,
      data: payload
    }));
  } catch (e) {
    console.warn('decode failed', e);
    eye.sawKeyframe = false;
  }
};

setInterval(() => {
  mbps = ((bytes - lastBytes) * 8 / 1e6).toFixed(1);
  lastBytes = bytes;
  const total = eyes[0].frames + eyes[1].frames;
  fps = total - lastFrames;
  lastFrames = total;
  stats.innerHTML =
    'received <b>' + received + '</b> frames &nbsp;|&nbsp; ' +
    'decoded <b>' + eyes[0].frames + '</b> / <b>' + eyes[1].frames + '</b> (L/R) &nbsp;|&nbsp; ' +
    '<b>' + fps + '</b> fps &nbsp;|&nbsp; <b>' + mbps + '</b> Mbps';
}, 1000);
</script>
</body>
</html>)HTML";
