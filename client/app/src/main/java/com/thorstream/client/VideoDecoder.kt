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

    var framesDecoded = 0L
        private set

    fun start(session: SessionInfo) {
        val mime = if (session.codec == Protocol.CODEC_HEVC) {
            MediaFormat.MIMETYPE_VIDEO_HEVC
        } else {
            MediaFormat.MIMETYPE_VIDEO_AVC
        }

        val format = MediaFormat.createVideoFormat(mime, session.width, session.height).apply {
            // SPS/PPS (or VPS/SPS/PPS) in Annex-B form. The host also repeats these
            // inline on every keyframe, so this is belt and braces.
            if (session.sequenceHeader.isNotEmpty()) {
                setByteBuffer("csd-0", ByteBuffer.wrap(session.sequenceHeader))
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            }
            setInteger(MediaFormat.KEY_PRIORITY, 0) // realtime
        }

        val decoder = MediaCodec.createDecoderByType(mime)
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

        decoder.configure(format, surface, null, 0)
        decoder.start()
        codec = decoder
        running = true
        Log.i(TAG, "decoder started: $mime ${session.width}x${session.height}")
    }

    /** Queues a reassembled frame. Safe to call from the receiver thread. */
    fun submit(frame: VideoFrame) {
        if (!running) return
        // If the decoder has fallen behind, dropping the backlog costs one glitch
        // and recovers latency; keeping it costs latency forever.
        if (pendingFrames.size > MAX_QUEUED_FRAMES) {
            pendingFrames.clear()
            Log.w(TAG, "decoder backlog exceeded; dropped queued frames")
        }
        pendingFrames.add(frame)
        pump()
    }

    private fun pump() {
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
