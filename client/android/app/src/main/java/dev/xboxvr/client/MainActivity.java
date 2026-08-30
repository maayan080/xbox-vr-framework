package dev.xboxvr.client;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.Locale;

/**
 * Phase 1 step 3a: receive the Xbox stream and decode it on Android.
 *
 * Deliberately not a VR app yet. This runs on an ordinary phone as well as on the Quest,
 * so decode and networking can be proven on ARM in seconds rather than through a headset
 * sideload each time. OpenXR comes next, on top of a decode path already known to work.
 *
 * Both eyes are shown side by side as flat video; stereo correctness is a later slice.
 */
public class MainActivity extends Activity implements StreamReceiver.Listener {

    private final SurfaceView[] surfaces = new SurfaceView[2];
    private final VideoDecoder[] decoders = { new VideoDecoder(), new VideoDecoder() };
    private final boolean[] surfaceReady = new boolean[2];

    private StreamReceiver receiver;
    private TextView statusView;
    private final Handler ui = new Handler(Looper.getMainLooper());

    private long lastFrames;
    private long lastBytes;
    private int refreshRate = 72;

    /**
     * This device's actual display refresh rate, reported to the host so it can match it.
     *
     * Quest headsets default to 72Hz but can be set to 90 or 120, and a phone may be
     * anything from 60 upward. Asking the display beats assuming.
     */
    private int detectRefreshRate() {
        try {
            float rate = getWindowManager().getDefaultDisplay().getRefreshRate();
            if (rate >= 30f && rate <= 240f) {
                return Math.round(rate);
            }
        } catch (Exception ignored) {
            // Fall through to the default rather than failing to start over a hint.
        }
        return 72;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFF0D0F17);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setLayoutParams(new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        for (int eye = 0; eye < 2; eye++) {
            final int index = eye;
            SurfaceView view = new SurfaceView(this);
            view.setLayoutParams(new LinearLayout.LayoutParams(0,
                    LinearLayout.LayoutParams.MATCH_PARENT, 1f));

            view.getHolder().addCallback(new SurfaceHolder.Callback() {
                @Override
                public void surfaceCreated(SurfaceHolder holder) {
                    // The decoder needs a real Surface, so it cannot be started until
                    // the view actually exists.
                    decoders[index].start(holder.getSurface(), 1920, 1088);
                    surfaceReady[index] = true;
                }

                @Override
                public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {}

                @Override
                public void surfaceDestroyed(SurfaceHolder holder) {
                    surfaceReady[index] = false;
                    decoders[index].stop();
                }
            });

            surfaces[eye] = view;
            row.addView(view);
        }

        statusView = new TextView(this);
        statusView.setTextColor(0xFFE6E9EF);
        statusView.setBackgroundColor(0xFF11141F);
        statusView.setPadding(24, 16, 24, 16);
        statusView.setText("starting…");

        root.addView(row);
        root.addView(statusView);
        setContentView(root);

        refreshRate = detectRefreshRate();
        receiver = new StreamReceiver(this, refreshRate);
        receiver.start();

        ui.post(statsTick);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        ui.removeCallbacks(statsTick);
        if (receiver != null) {
            receiver.stop();
        }
        for (VideoDecoder decoder : decoders) {
            decoder.stop();
        }
    }

    @Override
    public void onFrame(int eye, byte[] data, int length, boolean keyframe, long captureTimeUs,
                        int renderPoseSequence) {
        if (eye < 0 || eye > 1 || !surfaceReady[eye]) {
            return;
        }
        // Decoded on the receive thread: hopping to the UI thread would add a scheduling
        // delay to every frame for no benefit, since rendering goes straight to a Surface.
        decoders[eye].decode(data, length, keyframe, captureTimeUs);
    }

    @Override
    public void onStatus(String message) {
        ui.post(() -> statusView.setText(message));
    }

    private final Runnable statsTick = new Runnable() {
        @Override
        public void run() {
            StreamReceiver.Stats stats = receiver.stats;

            final long frames = stats.framesCompleted;
            final long bytes = stats.bytesReceived;
            final double mbps = (bytes - lastBytes) * 8.0 / 1_000_000.0;
            final long fps = frames - lastFrames;
            lastFrames = frames;
            lastBytes = bytes;

            String text;
            if (stats.fragmentsReceived == 0) {
                text = "Searching for the Xbox on this network…  (asking for "
                        + refreshRate + "fps)\n"
                        + "Make sure the console app is running and both are on the same Wi-Fi.";
            } else {
                text = String.format(Locale.US,
                        "%d fps  |  %.1f Mbps  |  frames %d  |  dropped %d  |  lost frags %d\n"
                                + "missing %d  |  decoded L %d  R %d   skipped L %d  R %d",
                        fps, mbps, frames, stats.framesDropped, stats.fragmentsLost,
                        stats.framesMissing,
                        decoders[0].framesDecoded, decoders[1].framesDecoded,
                        decoders[0].framesSkipped, decoders[1].framesSkipped);

                if (decoders[0].lastError != null) {
                    text += "\ndecoder error: " + decoders[0].lastError;
                }
            }

            statusView.setText(text);
            ui.postDelayed(this, 1000);
        }
    };
}
