package dev.xboxvr.vr;

import android.app.NativeActivity;
import android.graphics.SurfaceTexture;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;

import dev.xboxvr.client.PoseSender;
import dev.xboxvr.client.StreamReceiver;
import dev.xboxvr.client.VideoDecoder;

/**
 * The VR client's Java half: network, decode, and the SurfaceTextures the native OpenXR
 * renderer samples.
 *
 * Decoding stays in Java rather than moving to NDK AMediaCodec because this exact code is
 * already proven working on the 2D client. Reusing it means a problem in the headset is a
 * problem with OpenXR or the texture bridge, not with decode - which is the whole reason
 * the flat app was built first.
 */
public class VrActivity extends NativeActivity implements StreamReceiver.Listener {

    private static final String TAG = "XvrVrActivity";

    static {
        // Must match android.app.lib_name in the manifest.
        System.loadLibrary("xvrvr");
    }

    // Mirrors xvr::Status in the native code.
    private static final int STATUS_WAITING = 0;
    private static final int STATUS_RECEIVING_NO_KEYFRAME = 1;
    private static final int STATUS_DECODING = 2;

    private final VideoDecoder[] decoders = { new VideoDecoder(), new VideoDecoder() };
    private final SurfaceTexture[] surfaceTextures = new SurfaceTexture[2];
    private final Surface[] surfaces = new Surface[2];
    private final boolean[] frameAvailable = new boolean[2];

    private StreamReceiver receiver;
    private final PoseSender poseSender = new PoseSender();
    private volatile boolean texturesReady;
    // The pose belonging to the image currently on screen, per eye, recovered from the
    // decoder rather than from arrival order.
    private final int[] displayedRenderPoseSequence = new int[2];
    private volatile float[] controllerState;
    private long firstEyeArrivedAt;
    private volatile long unpairedFrames;

    // Counted so a silently disabled reprojection is visible rather than looking like
    // "tracking mysteriously stopped" - which is exactly how it presented last time.
    private volatile long reprojectionMisses;
    private volatile long reprojectionHits;

    private long latencySamples;
    private double latencyTotalMs;
    private double latencyMinMs;
    private double latencyMaxMs;
    private long lastLatencyReport;
    private boolean loggedTagSources;
    private long latencyLookupFailures;
    private long latencyImplausible;

    // Previous totals, so each report shows the last second rather than the whole session.
    private long lastFramesCompleted;
    private long lastFramesDropped;
    private long lastFragmentsLost;
    private long lastFramesMissing;
    private final long[] lastDecoded = new long[2];
    private final long[] lastSkipped = new long[2];
    private long lastUnpaired;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Quest headsets report their configured refresh rate here, which the host then
        // matches. Defaults vary between 72, 90 and 120 depending on the user's setting.
        int refreshRate = 72;
        try {
            float rate = getWindowManager().getDefaultDisplay().getRefreshRate();
            if (rate >= 30f && rate <= 240f) {
                refreshRate = Math.round(rate);
            }
        } catch (Exception ignored) {
            // Keep the default rather than failing to start over a hint.
        }

        Log.i(TAG, "requesting " + refreshRate + "fps");
        receiver = new StreamReceiver(this, refreshRate);
        receiver.start();
        poseSender.start();
    }

    /**
     * Called from the render thread every frame with the viewpoint OpenXR just reported.
     *
     * Sent immediately rather than posted to another thread: every millisecond between
     * sampling this pose and the host rendering against it is motion-to-photon latency.
     *
     * @param pose 7 floats: head position x/y/z then orientation x/y/z/w
     * @param eyes 22 floats: per eye, position[3], orientation[4], fov[4]
     */
    @SuppressWarnings("unused") // called from native
    public void submitPose(float[] pose, float[] eyes) {
        if (!poseSender.hasHost()) {
            if (receiver == null || receiver.hostAddress == null) {
                return;
            }
            poseSender.setHost(receiver.hostAddress);
            Log.i(TAG, "sending pose to " + receiver.hostAddress);
        }

        final float[] position = { pose[0], pose[1], pose[2] };
        final float[] orientation = { pose[3], pose[4], pose[5], pose[6] };
        poseSender.send(position, orientation, eyes, controllerState);
    }

    /**
     * Latest controller state from the native action system, already laid out to match the
     * wire format. Stored rather than sent directly because poses and controllers are
     * gathered at different points in the frame but belong in one packet.
     */
    @SuppressWarnings("unused") // called from native
    public void submitControllers(float[] state) {
        controllerState = state;
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (receiver != null) {
            receiver.stop();
        }
        poseSender.stop();
        for (VideoDecoder decoder : decoders) {
            decoder.stop();
        }
    }

    /**
     * Called once from the render thread with texture ids created in its GL context.
     *
     * The SurfaceTextures must wrap textures from that same context, or the decoder will
     * write into something the renderer cannot sample and the result is a silent black
     * image rather than an error.
     */
    @SuppressWarnings("unused") // called from native
    public void attachEyeTextures(int leftTextureId, int rightTextureId) {
        final int[] ids = { leftTextureId, rightTextureId };

        for (int eye = 0; eye < 2; eye++) {
            final int index = eye;
            surfaceTextures[eye] = new SurfaceTexture(ids[eye]);
            surfaceTextures[eye].setDefaultBufferSize(1920, 1088);
            surfaceTextures[eye].setOnFrameAvailableListener(texture -> {
                synchronized (frameAvailable) {
                    frameAvailable[index] = true;
                }
            });

            surfaces[eye] = new Surface(surfaceTextures[eye]);
            decoders[eye].start(surfaces[eye], 1920, 1088);
        }

        texturesReady = true;
        Log.i(TAG, "eye textures attached: " + leftTextureId + ", " + rightTextureId);
    }

    /**
     * Consumes any newly decoded frames into the GL textures. Must be called on the
     * render thread with the GL context current. Returns true if anything was updated.
     */
    @SuppressWarnings("unused") // called from native
    public boolean updateEyeTextures() {
        if (!texturesReady) {
            return false;
        }

        // Each eye is latched every frame, independently.
        //
        // An earlier version held both eyes until each had a new frame, so they could not
        // show different moments. That was a workaround for both eyes sharing one pose
        // tag; now each eye carries its own and reprojection corrects them separately, so
        // a one-frame difference between eyes is handled correctly rather than being
        // something to avoid.
        //
        // Gating on both was expensive: the eyes are separate streams and rarely arrive on
        // the same tick, so most display frames latched nothing at all - 91 decoded frames
        // per eye reached the screen as roughly 60. That shortfall is visible as uneven
        // cadence, which is precisely what the pairing was meant to prevent.
        synchronized (frameAvailable) {
            if (frameAvailable[0] != frameAvailable[1]) {
                unpairedFrames++;
            }
            frameAvailable[0] = false;
            frameAvailable[1] = false;
        }

        boolean updated = false;
        for (int eye = 0; eye < 2; eye++) {
            if (surfaceTextures[eye] == null) {
                continue;
            }
            try {
                // Latches the most recent frame, so any backlog is skipped rather than
                // played out late.
                surfaceTextures[eye].updateTexImage();
                updated = true;

                // Read the tag off the frame that just became visible, per eye.
                //
                // The two decoders run independently and each latches whatever frame it
                // has most recently, so the eyes are not always showing the same moment.
                // Reprojecting both with one eye's pose leaves the other warped by a small
                // shifting error - smooth in one eye, stuttering in the other.
                final int sequence = decoders[eye].lastRenderedPoseSequence;

                // Logged once so a decoder that does not carry the presentation timestamp
                // through to the surface is identifiable, rather than presenting as
                // "reprojection and latency both mysteriously do nothing".
                if (!loggedTagSources) {
                    loggedTagSources = true;
                    Log.i(TAG, "pose tag from decoder=" + sequence + ", from surface="
                            + VideoDecoder.poseSequenceFromTimestampNs(
                                    surfaceTextures[eye].getTimestamp()));
                }

                if (sequence > 0) {
                    if (sequence != displayedRenderPoseSequence[eye]) {
                        recordLatency(sequence);
                    }
                    displayedRenderPoseSequence[eye] = sequence;
                }
            } catch (Exception e) {
                Log.w(TAG, "updateTexImage failed: " + e.getMessage());
            }
        }
        return updated;
    }

    /** Reported to native so the headset can show which stage the pipeline reached. */
    @SuppressWarnings("unused") // called from native
    public int getStreamStatus() {
        if (receiver == null || receiver.stats.fragmentsReceived == 0) {
            return STATUS_WAITING;
        }
        if (!decoders[0].isDecoding()) {
            return STATUS_RECEIVING_NO_KEYFRAME;
        }
        return STATUS_DECODING;
    }

    @Override
    public void onFrame(int eye, byte[] data, int length, boolean keyframe, long captureTimeUs,
                        int renderPoseSequence) {
        if (!texturesReady || eye < 0 || eye > 1) {
            return;
        }
        // The pose sequence rides through the decoder as the presentation timestamp, so it
        // can be read back off the frame that is actually on screen. Using the sequence of
        // the most recently *arrived* frame would be wrong by however long the decoder
        // takes - warping each image by an error that shifts as frames flow.
        decoders[eye].decode(data, length, keyframe,
                renderPoseSequence * VideoDecoder.POSE_SEQUENCE_SCALE);
    }

    /**
     * Measures motion-to-photon latency for a frame reaching the display.
     *
     * The pose that produced this image was sampled here, so the elapsed time covers the
     * whole loop: pose out, host render, encode, network, decode, and display. No clock
     * synchronisation is involved because both ends are timed by this device.
     *
     * What it does not include is the time between the runtime predicting a display time
     * and the pose being sampled - so the true figure is slightly higher than this, not
     * lower.
     */
    private void recordLatency(int poseSequence) {
        final long sentAt = poseSender.sentAtNanos(poseSequence);
        if (sentAt == 0) {
            // Sparse, but never silent: a measurement that quietly does nothing is
            // indistinguishable from one that is not running.
            if (++latencyLookupFailures % 240 == 1) {
                Log.w(TAG, "latency unmeasurable: no record of pose " + poseSequence
                        + " (failures " + latencyLookupFailures + ")");
            }
            return;
        }

        final double ms = (System.nanoTime() - sentAt) / 1_000_000.0;
        if (ms <= 0.0 || ms > 2000.0) {
            // Implausible: the ring wrapped, or a sequence collided after a restart.
            if (++latencyImplausible % 240 == 1) {
                Log.w(TAG, "latency implausible: " + ms + "ms for pose " + poseSequence);
            }
            return;
        }

        latencySamples++;
        latencyTotalMs += ms;
        if (ms < latencyMinMs || latencyMinMs == 0.0) {
            latencyMinMs = ms;
        }
        if (ms > latencyMaxMs) {
            latencyMaxMs = ms;
        }

        // Reported once a second rather than per frame, and reset each time so the numbers
        // describe recent conditions instead of averaging away every change since launch.
        final long now = android.os.SystemClock.uptimeMillis();
        if (now - lastLatencyReport > 1000) {
            lastLatencyReport = now;
            Log.i(TAG, String.format(java.util.Locale.US,
                    "motion-to-photon: min %.1fms  avg %.1fms  max %.1fms  (%d frames)",
                    latencyMinMs, latencyTotalMs / latencySamples, latencyMaxMs,
                    latencySamples));

            // Reported alongside latency so a shortfall between what the host sends and
            // what reaches the screen is attributable rather than inferred. "arrived" is
            // what survived the network; "decoded" is what the codec actually produced;
            // "skipped" is what this client threw away because the decoder was behind.
            final StreamReceiver.Stats s = receiver.stats;
            Log.i(TAG, String.format(java.util.Locale.US,
                    "arrived %d (dropped %d, lost frags %d, missing %d)  "
                            + "decoded L %d R %d  skipped L %d R %d  unpaired %d",
                    s.framesCompleted - lastFramesCompleted,
                    s.framesDropped - lastFramesDropped,
                    s.fragmentsLost - lastFragmentsLost,
                    s.framesMissing - lastFramesMissing,
                    decoders[0].framesDecoded - lastDecoded[0],
                    decoders[1].framesDecoded - lastDecoded[1],
                    decoders[0].framesSkipped - lastSkipped[0],
                    decoders[1].framesSkipped - lastSkipped[1],
                    unpairedFrames - lastUnpaired));

            lastFramesCompleted = s.framesCompleted;
            lastFramesDropped = s.framesDropped;
            lastFragmentsLost = s.fragmentsLost;
            lastFramesMissing = s.framesMissing;
            lastDecoded[0] = decoders[0].framesDecoded;
            lastDecoded[1] = decoders[1].framesDecoded;
            lastSkipped[0] = decoders[0].framesSkipped;
            lastSkipped[1] = decoders[1].framesSkipped;
            lastUnpaired = unpairedFrames;
            latencySamples = 0;
            latencyTotalMs = 0.0;
            latencyMinMs = 0.0;
            latencyMaxMs = 0.0;
        }
    }

    /** Which pose the frame currently on screen for this eye was rendered against. */
    @SuppressWarnings("unused") // called from native
    public int getRenderPoseSequence(int eye) {
        if (eye < 0 || eye > 1) {
            return 0;
        }
        return displayedRenderPoseSequence[eye];
    }

    /** The eye data sent with a given sequence, or null if no longer held. */
    @SuppressWarnings("unused") // called from native
    public float[] lookupPose(int poseSequence) {
        final float[] pose = poseSender.lookupPose(poseSequence);

        if (pose == null) {
            reprojectionMisses++;
            // Logged sparsely: at 120Hz a per-frame message would bury everything else,
            // but total silence is how this hid last time.
            if (reprojectionMisses % 120 == 1) {
                Log.w(TAG, "reprojection unavailable: no history for pose " + poseSequence
                        + " (misses " + reprojectionMisses + ", hits " + reprojectionHits + ")");
            }
        } else {
            reprojectionHits++;
        }
        return pose;
    }

    @Override
    public void onStatus(String message) {
        Log.i(TAG, message);
    }
}
