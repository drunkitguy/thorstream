package com.thorstream.client

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.util.concurrent.ConcurrentLinkedQueue

/**
 * Hardware video decoder rendering straight to a Surface.
 *
 * Configured for latency rather than throughput: no output buffering beyond what
 * the codec requires, and every decoded frame is released to the display the
 * instant it appears.
 */
class VideoDecoder(private val surface: Surface) {

    var onError: ((String) -> Unit)? = null

    private var codec: MediaCodec? = null
    private val availableInputBuffers = ConcurrentLinkedQueue<Int>()
    private val pendingFrames = ConcurrentLinkedQueue<VideoFrame>()

    @Volatile
    private var running = false

    @Volatile
    private var sawKeyframe = false

    @Volatile
    private var renderedFrame = false

    var framesDecoded = 0L
        private set

    var skippedBeforeKeyframe = 0
        private set

    val hasKeyframe: Boolean get() = sawKeyframe

    /**
     * True once a decoded picture has been handed to the surface.
     *
     * [hasKeyframe] is not the same thing: it is set when a keyframe is accepted
     * for queueing, and that frame can still be dropped before it reaches the
     * codec. Only this says something was actually put on screen, which is what
     * anyone waiting for the black screen to end cares about.
     */
    val hasRenderedFrame: Boolean get() = renderedFrame

    /**
     * [requestedCodec] is the codec that was ASKED for, not `session.codec`. The
     * host currently echoes the request back in STARTED rather than reporting
     * what it configured, so that byte cannot be used to pick a decoder; a
     * host-side fix is in flight and this stays correct either way.
     */
    fun start(session: SessionInfo, requestedCodec: Byte) {
        val mime = CodecSupport.mimeFor(requestedCodec)
        val isAv1 = requestedCodec == Protocol.CODEC_AV1

        val format = MediaFormat.createVideoFormat(mime, session.width, session.height).apply {
            // SPS/PPS (or VPS/SPS/PPS) in Annex-B form. The host also repeats these
            // inline on every keyframe, so this is belt and braces.
            //
            // Never for AV1. What the host puts in that field is a bare
            // OBU_SEQUENCE_HEADER in low-overhead format, but csd-0 for AV1 is
            // defined as an AV1CodecConfigurationRecord, so the OBU is either
            // rejected or misparsed. Synthesising a real record would mean
            // writing a fixed profile/level/tier prefix, and the tier the
            // encoder picks changes with the bitrate - Main below ~12 Mbps, High
            // at 20 - so any constant here breaks the moment the bitrate slider
            // moves. The encoder sets repeatSeqHdr, so the sequence header
            // precedes every keyframe in the stream and MediaCodec configures
            // itself from that instead.
            if (!isAv1 && session.sequenceHeader.isNotEmpty()) {
                setByteBuffer("csd-0", ByteBuffer.wrap(session.sequenceHeader))
            }
            if (isAv1) {
                // AV1 signals colour inside the sequence header, which the
                // decoder only reaches at the first keyframe. Declaring it up
                // front stops a studio-range picture being stretched as if it
                // were full range in the meantime. The host's fullRange defaults
                // false, so LIMITED is the honest answer - never declare full.
                setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
                setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
                setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }
            setInteger(MediaFormat.KEY_PRIORITY, 0) // realtime
        }

        // By name for AV1 only. createDecoderByType hands back whichever decoder
        // the device lists first, and on a device that has both that can be the
        // software one - which will not hold 1080p60 and would be worse than the
        // H.264 it replaced. H.264 and HEVC keep the original path exactly.
        val hardwareName = if (isAv1) CodecSupport.hardwareDecoderName(mime) else null
        val decoder = if (hardwareName != null) {
            MediaCodec.createByCodecName(hardwareName)
        } else {
            MediaCodec.createDecoderByType(mime)
        }
        decoder.setCallback(object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(mc: MediaCodec, index: Int) {
                availableInputBuffers.add(index)
                pump()
            }

            override fun onOutputBufferAvailable(
                mc: MediaCodec,
                index: Int,
                info: MediaCodec.BufferInfo,
            ) {
                framesDecoded++
                // `true` renders to the surface immediately - no scheduling delay.
                try {
                    mc.releaseOutputBuffer(index, true)
                    renderedFrame = true
                } catch (e: IllegalStateException) {
                    Log.w(TAG, "releaseOutputBuffer after stop", e)
                }
            }

            override fun onOutputFormatChanged(mc: MediaCodec, format: MediaFormat) {
                Log.i(TAG, "decoder output format: $format")
            }

            override fun onError(mc: MediaCodec, e: MediaCodec.CodecException) {
                Log.e(TAG, "decoder error", e)
                onError?.invoke(e.diagnosticInfo ?: e.message ?: "decoder error")
            }
        })

        try {
            decoder.configure(format, surface, null, 0)
            decoder.start()
        } catch (e: Exception) {
            // codec is only assigned once both of these succeed, so stop() could
            // never reach this instance to release it - an unsupported format
            // would leak a hardware codec on every retry.
            decoder.release()
            throw e
        }
        this.codec = decoder
        running = true
        Log.i(TAG, "decoder started: ${decoder.name} $mime ${session.width}x${session.height}")
    }

    /**
     * Queues a reassembled frame. Safe to call from the receiver thread.
     *
     * Frames are ignored until the first keyframe arrives. Feeding a decoder
     * P-frames that reference a keyframe it never saw produces no output at all,
     * which looks exactly like a dead stream.
     */
    fun submit(frame: VideoFrame) {
        if (!running) return
        if (!sawKeyframe) {
            if (!frame.isKeyframe) {
                skippedBeforeKeyframe++
                return
            }
            sawKeyframe = true
            Log.i(TAG, "first keyframe after skipping $skippedBeforeKeyframe frames")
        }
        // If the decoder has fallen behind, dropping the backlog costs one glitch
        // and recovers latency; keeping it costs latency forever.
        if (pendingFrames.size > MAX_QUEUED_FRAMES) {
            pendingFrames.clear()
            Log.w(TAG, "decoder backlog exceeded; dropped queued frames")
        }
        pendingFrames.add(frame)
        pump()
    }

    // pump() is entered from both the receiver thread (submit) and the codec's
    // callback thread. Without this lock the peek/poll pair races and two
    // threads can take the same frame.
    private val pumpLock = Any()

    private fun pump() = synchronized(pumpLock) {
        val decoder = codec ?: return
        while (running) {
            val frame = pendingFrames.peek() ?: return
            val index = availableInputBuffers.poll() ?: return
            pendingFrames.poll()

            try {
                val buffer: ByteBuffer = decoder.getInputBuffer(index) ?: continue
                buffer.clear()
                if (buffer.capacity() < frame.data.size) {
                    Log.w(TAG, "input buffer too small for ${frame.data.size} bytes; dropping frame")
                    decoder.queueInputBuffer(index, 0, 0, 0, 0)
                    continue
                }
                // The frame goes in whole and untouched. For AV1 that is one
                // complete temporal unit, opened by its OBU_TEMPORAL_DELIMITER,
                // and the delimiter is part of what the decoder parses - do not
                // be tempted to strip it. BUFFER_FLAG_CODEC_CONFIG is likewise
                // never set: an ordinary frame is not configuration data even
                // when the encoder has repeated the sequence header inside it.
                buffer.put(frame.data)
                val flags = if (frame.isKeyframe) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0
                decoder.queueInputBuffer(index, 0, frame.data.size, frame.timestampMicros, flags)
            } catch (e: IllegalStateException) {
                Log.w(TAG, "queueInputBuffer failed", e)
                return
            }
        }
    }

    fun stop() {
        running = false
        pendingFrames.clear()
        availableInputBuffers.clear()
        codec?.let {
            try {
                it.stop()
            } catch (e: IllegalStateException) {
                Log.w(TAG, "stop failed", e)
            }
            it.release()
        }
        codec = null
    }

    private companion object {
        const val TAG = "VideoDecoder"
        const val MAX_QUEUED_FRAMES = 4
    }
}
