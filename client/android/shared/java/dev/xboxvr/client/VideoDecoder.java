package dev.xboxvr.client;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.ArrayDeque;

/**
 * Hardware H.264 decode straight to a Surface, driven asynchronously.
 *
 * Decoding to a Surface rather than to byte buffers keeps the frame on the GPU: there is
 * no readback to system memory, which would cost both latency and bandwidth on a mobile
 * part.
 *
 * The decoder runs in MediaCodec's asynchronous mode, so a finished frame is released to
 * the Surface the instant the codec reports it. Polling for output only when new data
 * arrives - the obvious synchronous shape - leaves each decoded frame waiting for the next
 * one to show up before anyone collects it, which is latency spent for nothing.
 */
public class VideoDecoder {

    private static final String TAG = "XvrDecoder";
    private static final String MIME = MediaFormat.MIMETYPE_VIDEO_AVC;

    /**
     * Scale used to carry the pose sequence through the decoder as a timestamp.
     *
     * The decoder is the only thing that knows when a frame actually reaches the screen -
     * output lags input by a frame or more. Tagging each frame on the way in and reading
     * the tag back when it is released is what lets reprojection use the pose the displayed
     * image was really rendered with.
     */
    public static final long POSE_SEQUENCE_SCALE = 1000L;

    /** Recovers the pose sequence from a SurfaceTexture timestamp, which is nanoseconds. */
    public static int poseSequenceFromTimestampNs(long timestampNs) {
        return (int) (timestampNs / (POSE_SEQUENCE_SCALE * 1000L));
    }

    private static final class PendingFrame {
        final byte[] data;
        final int length;
        final boolean keyframe;
        final long presentationTimeUs;

        PendingFrame(byte[] data, int length, boolean keyframe, long presentationTimeUs) {
            // Copied because the caller reuses its reassembly buffer immediately.
            this.data = new byte[length];
            System.arraycopy(data, 0, this.data, 0, length);
            this.length = length;
            this.keyframe = keyframe;
            this.presentationTimeUs = presentationTimeUs;
        }
    }

    private MediaCodec codec;
    private HandlerThread codecThread;
    private volatile boolean configured;
    private volatile boolean sawKeyframe;

    // Frames waiting for an input buffer, and input buffers waiting for a frame. Only one
    // of these is ever non-empty.
    private final ArrayDeque<PendingFrame> pendingFrames = new ArrayDeque<>();
    private final ArrayDeque<Integer> freeInputBuffers = new ArrayDeque<>();
    private final Object lock = new Object();

    public volatile long framesDecoded;
    public volatile long framesSkipped;
    public volatile String lastError;

    /** Pose sequence of the most recent frame the decoder rendered to its Surface. */
    public volatile int lastRenderedPoseSequence;

    public boolean start(Surface surface, int width, int height) {
        try {
            codecThread = new HandlerThread("xvr-decode");
            codecThread.start();

            codec = MediaCodec.createDecoderByType(MIME);
            codec.setCallback(new MediaCodec.Callback() {
                @Override
                public void onInputBufferAvailable(MediaCodec mc, int index) {
                    handleInputBuffer(index);
                }

                @Override
                public void onOutputBufferAvailable(MediaCodec mc, int index,
                                                    MediaCodec.BufferInfo info) {
                    // Taken from the decoder's own output info: authoritative, unlike
                    // reading it back off the SurfaceTexture afterwards.
                    lastRenderedPoseSequence = (int) (info.presentationTimeUs
                            / POSE_SEQUENCE_SCALE);
                    try {
                        // `true` renders to the Surface. Released immediately rather than
                        // held for a later poll - this is the whole point of async mode.
                        mc.releaseOutputBuffer(index, true);
                        framesDecoded++;
                    } catch (Exception e) {
                        lastError = e.getMessage();
                    }
                }

                @Override
                public void onError(MediaCodec mc, MediaCodec.CodecException e) {
                    lastError = e.getMessage();
                    Log.e(TAG, "decoder error", e);
                    sawKeyframe = false;
                }

                @Override
                public void onOutputFormatChanged(MediaCodec mc, MediaFormat format) {
                    Log.i(TAG, "output format: " + format);
                }
            }, new Handler(codecThread.getLooper()));

            MediaFormat format = MediaFormat.createVideoFormat(MIME, width, height);
            // Tells the decoder not to build a reordering queue waiting for frames that
            // will never arrive in a live stream.
            format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
            format.setInteger(MediaFormat.KEY_PRIORITY, 0); // realtime

            codec.configure(format, surface, null, 0);
            codec.start();

            configured = true;
            sawKeyframe = false;
            Log.i(TAG, "decoder started " + width + "x" + height + " (async)");
            return true;
        } catch (Exception e) {
            lastError = e.getMessage();
            Log.e(TAG, "decoder start failed", e);
            return false;
        }
    }

    public void stop() {
        configured = false;
        if (codec != null) {
            try {
                codec.stop();
                codec.release();
            } catch (Exception ignored) {
                // Releasing a codec that already faulted throws; nothing useful to do.
            }
            codec = null;
        }
        if (codecThread != null) {
            codecThread.quitSafely();
            codecThread = null;
        }
        synchronized (lock) {
            pendingFrames.clear();
            freeInputBuffers.clear();
        }
    }

    /** Feeds a waiting frame into a buffer, or parks the buffer until one arrives. */
    private void handleInputBuffer(int index) {
        PendingFrame frame;
        synchronized (lock) {
            frame = pendingFrames.poll();
            if (frame == null) {
                freeInputBuffers.add(index);
                return;
            }
        }
        submitToCodec(index, frame);
    }

    private void submitToCodec(int index, PendingFrame frame) {
        try {
            ByteBuffer input = codec.getInputBuffer(index);
            if (input == null) {
                framesSkipped++;
                return;
            }
            input.clear();
            input.put(frame.data, 0, frame.length);
            codec.queueInputBuffer(index, 0, frame.length, frame.presentationTimeUs,
                    frame.keyframe ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0);
        } catch (Exception e) {
            lastError = e.getMessage();
            framesSkipped++;
        }
    }

    /**
     * Feeds one complete frame. Returns false if it was skipped rather than queued.
     *
     * Frames before the first keyframe are discarded: a decoder cannot start mid-GOP, and
     * feeding it delta frames produces errors rather than pictures.
     *
     * @param presentationTimeUs must be poseSequence * POSE_SEQUENCE_SCALE, so the pose
     *     can be recovered when the frame is displayed.
     */
    public boolean decode(byte[] data, int length, boolean keyframe, long presentationTimeUs) {
        if (!configured) {
            return false;
        }

        if (!sawKeyframe) {
            if (!keyframe) {
                framesSkipped++;
                return false;
            }
            sawKeyframe = true;
        }

        final PendingFrame frame = new PendingFrame(data, length, keyframe, presentationTimeUs);

        Integer bufferIndex;
        synchronized (lock) {
            bufferIndex = freeInputBuffers.poll();
            if (bufferIndex == null) {
                // No buffer free: the decoder is behind. Keep only the newest frame -
                // queueing would trade latency for images that are already stale, and a
                // backlog here is exactly what makes a stream feel late.
                pendingFrames.clear();
                pendingFrames.add(frame);
                framesSkipped++;
                return false;
            }
        }

        submitToCodec(bufferIndex, frame);
        return true;
    }

    /** True once a keyframe has been seen and decoding has begun. */
    public boolean isDecoding() {
        return configured && sawKeyframe;
    }
}
