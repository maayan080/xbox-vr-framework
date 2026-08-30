package dev.xboxvr.client;

import android.util.Log;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.util.Arrays;

/**
 * Receives video fragments and reassembles frames.
 *
 * Mirrors the reassembly rules in docs/protocol.md: nothing is retransmitted, a frame
 * superseded by a newer one is abandoned immediately rather than held, and decoding only
 * begins once a keyframe has arrived.
 */
public class StreamReceiver {

    private static final String TAG = "XvrReceiver";

    public interface Listener {
        /** A complete frame for one eye. Called on the receive thread. */
        void onFrame(int eye, byte[] data, int length, boolean keyframe, long captureTimeUs,
                     int renderPoseSequence);
        void onStatus(String message);
    }

    /** Where the video is coming from, learned from the first packet that arrives. */
    public volatile InetAddress hostAddress;

    public static final class Stats {
        public volatile long fragmentsReceived;
        public volatile long bytesReceived;
        public volatile long framesCompleted;
        public volatile long framesDropped;
        public volatile long fragmentsLost;
        // Keyframes asked for after a frame failed to assemble. Rising means packet loss.
        public volatile long keyframeRequests;
        // Gaps in the frame index of delivered frames. Counted separately because a frame
        // that never begins reassembling is invisible to every other counter here - which
        // is exactly how a bug that dropped every other frame went unnoticed.
        public volatile long framesMissing;
    }

    public final Stats stats = new Stats();

    private final Listener listener;
    private final int requestedFrameRate;
    private volatile boolean running;
    private Thread thread;
    private DatagramSocket socket;

    // Rate limit for keyframe requests. A keyframe is many times larger than a P-frame, so
    // answering every dropped frame during a bad patch would put the most data on the link
    // exactly when it is already losing packets, and turn a brief smear into a sustained
    // one. One in flight per interval is enough: the request only has to survive once.
    private static final long KEYFRAME_REQUEST_INTERVAL_MS = 150;
    private long lastKeyframeRequestAt = 0;

    // When video was last seen, used to decide whether to resume announcing.
    private volatile long lastFragmentAt;
    private boolean announcing;

    /**
     * @param requestedFrameRate this device's display refresh rate. The host cannot guess
     *     it — most Quests default to 72Hz but the user may have selected 90 or 120, and
     *     guessing wrong means either judder or wasted encode capacity.
     */
    public StreamReceiver(Listener listener, int requestedFrameRate) {
        this.listener = listener;
        this.requestedFrameRate = requestedFrameRate;
    }

    public void start() {
        running = true;
        thread = new Thread(this::run, "xvr-receive");
        thread.start();
    }

    public void stop() {
        running = false;
        if (socket != null) {
            socket.close();
        }
        if (thread != null) {
            thread.interrupt();
        }
    }

    // Per-eye reassembly state.
    private static final class Assembler {
        long currentFrame = -1;
        long lastDelivered = -1;
        byte[] buffer = new byte[0];
        boolean[] received = new boolean[0];
        int fragmentCount;
        int haveCount;
        int finalSize;
        boolean keyframe;
        long captureTimeUs;
        int renderPoseSequence;
    }

    private void run() {
        Assembler[] assemblers = { new Assembler(), new Assembler() };
        Protocol.VideoHeader header = new Protocol.VideoHeader();

        try {
            socket = new DatagramSocket(null);
            socket.setReuseAddress(true);
            socket.setBroadcast(true);
            // A keyframe arrives as a burst of fragments; a small buffer would have the
            // kernel drop them before this thread ever sees them.
            socket.setReceiveBufferSize(4 * 1024 * 1024);
            socket.bind(new InetSocketAddress(Protocol.VIDEO_PORT));

            listener.onStatus("Listening on UDP " + Protocol.VIDEO_PORT);

            Thread announcer = new Thread(this::announce, "xvr-discovery");
            announcer.setDaemon(true);
            announcer.start();

            byte[] packetBuffer = new byte[2048];
            DatagramPacket packet = new DatagramPacket(packetBuffer, packetBuffer.length);

            while (running) {
                packet.setLength(packetBuffer.length);
                socket.receive(packet);
                final int length = packet.getLength();

                if (Protocol.isHandshakeAck(packetBuffer, length)) {
                    listener.onStatus("Host accepted connection");
                    continue;
                }

                if (!Protocol.parseVideoHeader(packetBuffer, length, header)) {
                    continue;
                }

                // Learned rather than configured: pose goes back to whoever sent video.
                if (hostAddress == null) {
                    hostAddress = packet.getAddress();
                }

                stats.fragmentsReceived++;
                stats.bytesReceived += length;
                lastFragmentAt = android.os.SystemClock.uptimeMillis();

                handleFragment(assemblers[header.eye], header, packetBuffer, length);
            }
        } catch (Exception e) {
            if (running) {
                Log.e(TAG, "receive failed", e);
                listener.onStatus("Receive failed: " + e.getMessage());
            }
        }
    }

    private void handleFragment(Assembler assembler, Protocol.VideoHeader header,
                                byte[] packetBuffer, int length) {
        // Late fragment for a frame already finished or abandoned. Its moment has passed.
        if (header.frameIndex < assembler.currentFrame) {
            return;
        }

        // The `fragmentCount == 0` half is essential, not defensive. After a frame
        // completes, currentFrame advances to the next index while fragmentCount is
        // cleared - so without it, the very next frame's fragments match currentFrame,
        // skip this reset, and are then rejected below for exceeding a fragmentCount of
        // zero. That silently discards every other frame, which starves the decoder of
        // P-frames and corrupts motion while leaving the statistics looking clean.
        if (header.frameIndex != assembler.currentFrame || assembler.fragmentCount == 0) {
            // A newer frame starting means the previous one will never complete. Drop it
            // now: holding it adds latency and it can no longer be shown on time.
            if (assembler.fragmentCount > 0 && assembler.haveCount < assembler.fragmentCount) {
                stats.fragmentsLost += assembler.fragmentCount - assembler.haveCount;
                stats.framesDropped++;

                // Everything decoded from here references a frame we never fully received,
                // so the picture stays smeared until an IDR arrives. Left alone that lasts
                // until the next scheduled keyframe - a whole GOP, about a second - which is
                // the tearing and smearing that looks like bad Wi-Fi. Ask for one now.
                requestKeyframe(assembler.lastDelivered);
            }

            assembler.currentFrame = header.frameIndex;
            assembler.fragmentCount = header.fragmentCount;
            assembler.haveCount = 0;
            assembler.finalSize = 0;
            assembler.keyframe = header.isKeyframe();
            assembler.captureTimeUs = header.captureTimeUs;
            assembler.renderPoseSequence = header.renderPoseSequence;

            final int needed = header.fragmentCount * Protocol.FRAGMENT_PAYLOAD_BYTES;
            if (assembler.buffer.length < needed) {
                assembler.buffer = new byte[needed];
            }
            if (assembler.received.length < header.fragmentCount) {
                assembler.received = new boolean[header.fragmentCount];
            }
            Arrays.fill(assembler.received, 0, header.fragmentCount, false);
        }

        if (header.fragmentIndex >= assembler.fragmentCount
                || assembler.received[header.fragmentIndex]) {
            return;
        }

        final int payloadSize = length - Protocol.VIDEO_HEADER_BYTES;
        final int offset = header.fragmentIndex * Protocol.FRAGMENT_PAYLOAD_BYTES;
        System.arraycopy(packetBuffer, Protocol.VIDEO_HEADER_BYTES, assembler.buffer, offset,
                payloadSize);

        assembler.received[header.fragmentIndex] = true;
        assembler.haveCount++;

        // The last fragment is usually short, so the true frame size is only known when
        // it arrives.
        if (header.isFinalFragment()) {
            assembler.finalSize = offset + payloadSize;
        }

        if (assembler.haveCount == assembler.fragmentCount && assembler.finalSize > 0) {
            if (assembler.lastDelivered >= 0 && header.frameIndex > assembler.lastDelivered + 1) {
                stats.framesMissing += header.frameIndex - assembler.lastDelivered - 1;
            }
            assembler.lastDelivered = header.frameIndex;

            stats.framesCompleted++;
            listener.onFrame(header.eye, assembler.buffer, assembler.finalSize,
                    assembler.keyframe, assembler.captureTimeUs, assembler.renderPoseSequence);

            assembler.currentFrame = header.frameIndex + 1;
            assembler.fragmentCount = 0;
            assembler.haveCount = 0;
            assembler.finalSize = 0;
        }
    }

    /** Asks the host for an IDR, no more often than the rate limit allows. */
    private void requestKeyframe(long lastGoodFrameIndex) {
        final DatagramSocket s = socket;
        final InetAddress host = hostAddress;
        if (s == null || s.isClosed() || host == null) {
            return;
        }

        final long now = android.os.SystemClock.uptimeMillis();
        if (now - lastKeyframeRequestAt < KEYFRAME_REQUEST_INTERVAL_MS) {
            return;
        }
        lastKeyframeRequestAt = now;

        try {
            final byte[] message = Protocol.buildKeyframeRequest(lastGoodFrameIndex);
            s.send(new DatagramPacket(message, message.length, host, Protocol.DISCOVERY_PORT));
            stats.keyframeRequests++;
        } catch (Exception ignored) {
            // Best effort. The request itself travels over the same lossy link it is trying
            // to recover from, so it is expected to go missing sometimes - the next dropped
            // frame sends another.
        }
    }

    /**
     * Announces on the local subnet until video arrives, so neither end has to be told
     * the other's address. Sent from the video socket, so the host can simply reply to
     * the source it heard from.
     */
    private void announce() {
        final byte[] handshake = Protocol.buildHandshake(1920, 1088, requestedFrameRate);

        while (running) {
            try {
                // Announce whenever video is not currently arriving, not merely until the
                // first packet ever seen.
                //
                // Stopping permanently after the first frame meant that if the host was
                // ever restarted, this client fell silent forever and the only cure was
                // restarting it too - which is exactly what someone trying this for the
                // first time will do, in the wrong order, and conclude it does not work.
                final long sinceLastFrame =
                        android.os.SystemClock.uptimeMillis() - lastFragmentAt;
                final boolean streamIdle = lastFragmentAt == 0 || sinceLastFrame > 2000;

                if (streamIdle && socket != null && !socket.isClosed()) {
                    DatagramPacket packet = new DatagramPacket(handshake, handshake.length,
                            InetAddress.getByName("255.255.255.255"), Protocol.DISCOVERY_PORT);
                    socket.send(packet);

                    if (!announcing) {
                        announcing = true;
                        listener.onStatus(lastFragmentAt == 0
                                ? "Searching for the host..."
                                : "Stream lost - searching again...");
                    }
                } else if (announcing) {
                    announcing = false;
                    // A reconnect starts a new stream mid-GOP, so the decoders must wait
                    // for a keyframe rather than trying to continue from where they were.
                    listener.onStatus("Connected");
                }

                Thread.sleep(500);
            } catch (InterruptedException e) {
                return;
            } catch (Exception e) {
                Log.w(TAG, "announce failed: " + e.getMessage());
                try {
                    Thread.sleep(1000);
                } catch (InterruptedException ignored) {
                    return;
                }
            }
        }
    }
}
