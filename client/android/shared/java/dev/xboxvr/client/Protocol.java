package dev.xboxvr.client;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * The wire contract, mirrored from framework/include/xvr/Protocol.h.
 *
 * This is the second implementation of the same format, so the two must be kept in step.
 * docs/protocol.md is the specification both follow; neither side may change the layout
 * without changing the other and bumping the version.
 */
public final class Protocol {

    public static final int MAGIC = 0x31525658; // 'XVR1' little-endian

    // MUST match kProtocolVersion in framework/include/xvr/Protocol.h. The two cannot
    // share a header, so this is the one place the contract is duplicated - bumping one
    // and not the other silently drops every packet, which looks like a dead network
    // rather than a version mismatch.
    //
    // Version 2 added renderPoseSequence to the video fragment header.
    // Version 3 appended controller state to PoseUpdate.
    public static final byte VERSION = 3;

    public static final byte TYPE_VIDEO_FRAGMENT = 0x01;
    public static final byte TYPE_POSE_UPDATE = 0x02;
    public static final byte TYPE_HANDSHAKE = 0x03;
    public static final byte TYPE_HANDSHAKE_ACK = 0x04;
    public static final byte TYPE_KEYFRAME_REQUEST = 0x05;
    public static final byte TYPE_HEARTBEAT = 0x06;

    public static final int FLAG_KEYFRAME = 1;
    public static final int FLAG_FINAL_FRAGMENT = 2;

    public static final int VIDEO_HEADER_BYTES = 32;
    public static final int FRAGMENT_PAYLOAD_BYTES = 1200;

    public static final int DISCOVERY_PORT = 9943;
    public static final int VIDEO_PORT = 9944;

    private Protocol() {}

    /** Parsed video fragment header. Fields mirror VideoFragmentHeader exactly. */
    public static final class VideoHeader {
        public int eye;
        public int flags;
        public int fragmentIndex;
        public int fragmentCount;
        public long frameIndex;
        public long captureTimeUs;
        /** Which pose this frame was rendered against, for reprojection. */
        public int renderPoseSequence;

        public boolean isKeyframe() { return (flags & FLAG_KEYFRAME) != 0; }
        public boolean isFinalFragment() { return (flags & FLAG_FINAL_FRAGMENT) != 0; }
    }

    /**
     * Returns false for anything that is not a valid packet of ours, so an unrelated
     * broadcast on the LAN is never interpreted as video.
     */
    public static boolean parseVideoHeader(byte[] data, int length, VideoHeader out) {
        if (length < VIDEO_HEADER_BYTES) {
            return false;
        }

        ByteBuffer buffer = ByteBuffer.wrap(data, 0, length).order(ByteOrder.LITTLE_ENDIAN);
        if (buffer.getInt() != MAGIC) {
            return false;
        }
        if (buffer.get() != TYPE_VIDEO_FRAGMENT) {
            return false;
        }
        if (buffer.get() != VERSION) {
            return false;
        }
        buffer.getShort(); // reserved

        out.eye = buffer.get() & 0xFF;
        out.flags = buffer.get() & 0xFF;
        out.fragmentIndex = buffer.getShort() & 0xFFFF;
        out.fragmentCount = buffer.getShort() & 0xFFFF;
        buffer.getShort(); // reserved
        out.frameIndex = buffer.getInt() & 0xFFFFFFFFL;
        out.captureTimeUs = buffer.getLong();
        out.renderPoseSequence = buffer.getInt();

        return out.eye <= 1 && out.fragmentCount > 0;
    }

    /** Builds the announcement the host replies to. */
    public static byte[] buildHandshake(int width, int height, int frameRate) {
        ByteBuffer buffer = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
        buffer.putInt(MAGIC);
        buffer.put(TYPE_HANDSHAKE);
        buffer.put(VERSION);
        buffer.putShort((short) 0);
        buffer.putShort((short) width);
        buffer.putShort((short) height);
        buffer.put((byte) frameRate);
        buffer.put((byte) 0);
        buffer.putShort((short) VERSION);
        return buffer.array();
    }

    public static byte[] buildKeyframeRequest(long lastGoodFrameIndex) {
        ByteBuffer buffer = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
        buffer.putInt(MAGIC);
        buffer.put(TYPE_KEYFRAME_REQUEST);
        buffer.put(VERSION);
        buffer.putShort((short) 0);
        buffer.putInt((int) lastGoodFrameIndex);
        buffer.putInt(0);
        return buffer.array();
    }

    /** True if this packet is a HandshakeAck, meaning the host has accepted us. */
    public static boolean isHandshakeAck(byte[] data, int length) {
        if (length < 8) {
            return false;
        }
        ByteBuffer buffer = ByteBuffer.wrap(data, 0, length).order(ByteOrder.LITTLE_ENDIAN);
        return buffer.getInt() == MAGIC && buffer.get() == TYPE_HANDSHAKE_ACK;
    }
}
