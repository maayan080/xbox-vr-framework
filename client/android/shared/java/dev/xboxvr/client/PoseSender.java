package dev.xboxvr.client;

import android.util.Log;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Sends the headset's viewpoint to the host, once per rendered frame.
 *
 * Poses are never queued or batched: a pose is worthless the moment a newer one exists,
 * so anything not sent immediately should simply be dropped.
 */
public class PoseSender {

    private static final String TAG = "XvrPose";

    // Poses are kept after sending so an arriving frame can be matched to the exact pose
    // it was rendered against. Without that, reprojection cannot know how far the head has
    // moved since, and the frame can only be shown stale.
    //
    // A power-of-two ring indexed by sequence: at 120Hz this holds about two seconds, far
    // longer than any frame's round trip, and costs nothing to maintain.
    private static final int HISTORY_SIZE = 256;
    private final float[][] history = new float[HISTORY_SIZE][];
    private final int[] historySequence = new int[HISTORY_SIZE];
    // When each pose was sampled, in this device's clock.
    private final long[] historySentAt = new long[HISTORY_SIZE];

    private DatagramSocket socket;
    private InetAddress host;
    private int sequence;

    public volatile long posesSent;
    public volatile String lastError;

    /**
     * The eye data sent with the given sequence, or null if it has been overwritten.
     * Returns 22 floats: per eye, position[3], orientation[4], fov[4].
     */
    public synchronized float[] lookupPose(int poseSequence) {
        final int slot = poseSequence & (HISTORY_SIZE - 1);
        if (historySequence[slot] != poseSequence) {
            return null;
        }
        return history[slot];
    }

    /** The most recently sent sequence number. */
    public synchronized int currentSequence() {
        return sequence;
    }

    /**
     * When the given pose was sampled, in System.nanoTime(), or 0 if no longer held.
     *
     * This is what makes end-to-end latency measurable without synchronising clocks with
     * the host: a pose leaves here, the host renders against it, and the resulting image
     * comes back and reaches the display. Both ends of that journey are timed by this
     * device, so the difference is true motion-to-photon latency with no clock offset to
     * estimate and no error from assuming symmetric network delay.
     */
    public synchronized long sentAtNanos(int poseSequence) {
        final int slot = poseSequence & (HISTORY_SIZE - 1);
        if (historySequence[slot] != poseSequence) {
            return 0;
        }
        return historySentAt[slot];
    }

    /** The host address is learned from the stream, so nothing has to be configured. */
    public void setHost(InetAddress address) {
        this.host = address;
    }

    public boolean hasHost() {
        return host != null;
    }

    public void start() {
        try {
            socket = new DatagramSocket();
        } catch (Exception e) {
            lastError = e.getMessage();
            Log.e(TAG, "could not open pose socket", e);
        }
    }

    public void stop() {
        if (socket != null) {
            socket.close();
            socket = null;
        }
    }

    /** 19 floats per hand, laid out to match ControllerState on the wire. */
    public static final int CONTROLLER_FLOATS = 19;

    /**
     * @param headPosition x/y/z metres
     * @param headOrientation quaternion x/y/z/w
     * @param eyeData 2 x (position[3], orientation[4], fov[4]) = 22 floats, or null
     * @param controllerData 2 x CONTROLLER_FLOATS, or null when no controllers are present
     */
    public void send(float[] headPosition, float[] headOrientation, float[] eyeData,
                     float[] controllerData) {
        if (socket == null || host == null) {
            return;
        }

        try {
            final int thisSequence;
            synchronized (this) {
                thisSequence = ++sequence;
                if (eyeData != null) {
                    final int slot = thisSequence & (HISTORY_SIZE - 1);
                    history[slot] = eyeData.clone();
                    historySequence[slot] = thisSequence;
                    historySentAt[slot] = System.nanoTime();
                }
            }

            final ByteBuffer buffer = ByteBuffer.allocate(292).order(ByteOrder.LITTLE_ENDIAN);
            buffer.putInt(Protocol.MAGIC);
            buffer.put(Protocol.TYPE_POSE_UPDATE);
            buffer.put(Protocol.VERSION);
            buffer.putShort((short) 0);

            // Microseconds, matching the host's expectation.
            buffer.putLong(System.nanoTime() / 1000L);
            buffer.putInt(thisSequence);
            buffer.putInt(eyeData != null ? 1 : 0); // PoseFlag_HasEyeData

            for (int i = 0; i < 3; i++) {
                buffer.putFloat(headPosition[i]);
            }
            for (int i = 0; i < 4; i++) {
                buffer.putFloat(headOrientation[i]);
            }

            for (int i = 0; i < 22; i++) {
                buffer.putFloat(eyeData != null ? eyeData[i] : 0f);
            }

            // The buttons slot carries a bit mask, so its bits are copied verbatim rather
            // than converted - a 32-bit mask would not survive a float round trip.
            for (int hand = 0; hand < 2; hand++) {
                final int base = hand * CONTROLLER_FLOATS;
                for (int i = 0; i < CONTROLLER_FLOATS; i++) {
                    final float value = controllerData != null ? controllerData[base + i] : 0f;
                    if (i == 0) {
                        buffer.putInt(Float.floatToRawIntBits(value));
                    } else {
                        buffer.putFloat(value);
                    }
                }
            }

            final byte[] bytes = buffer.array();
            socket.send(new DatagramPacket(bytes, bytes.length, host, Protocol.DISCOVERY_PORT));
            posesSent++;
        } catch (Exception e) {
            // Deliberately not retried: by the time a retry landed, a newer pose would
            // already exist and this one would be worse than useless.
            lastError = e.getMessage();
        }
    }
}
