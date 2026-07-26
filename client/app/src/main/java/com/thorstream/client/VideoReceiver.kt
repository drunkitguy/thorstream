package com.thorstream.client

import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.concurrent.thread

/** A frame that has been fully reassembled from its UDP fragments. */
class VideoFrame(val data: ByteArray, val isKeyframe: Boolean, val timestampMicros: Long)

/**
 * Receives fragmented video over UDP and reassembles whole frames.
 *
 * There is no retransmission by design: a frame that arrives late is worse than
 * useless, because showing it means showing stale gameplay. Incomplete frames are
 * dropped as soon as a newer one completes.
 */
class VideoReceiver(private val onFrame: (VideoFrame) -> Unit) {

    var onFrameLost: (() -> Unit)? = null

    private var socket: DatagramSocket? = null
    private var worker: Thread? = null

    @Volatile
    private var running = false

    private val pending = HashMap<Int, Array<ByteArray?>>()
    private val keyframeFlags = HashMap<Int, Boolean>()
    private val timestamps = HashMap<Int, Long>()
    private var newestCompleted = -1

    var datagramsReceived = 0L
        private set
    var framesCompleted = 0L
        private set
    var framesDropped = 0L
        private set

    /** Binds an ephemeral port and returns it, for sending in START. */
    fun bind(): Int {
        val s = DatagramSocket(0)
        s.receiveBufferSize = 4 * 1024 * 1024
        s.soTimeout = 1000
        socket = s
        return s.localPort
    }

    fun start() {
        val s = socket ?: error("bind() must be called before start()")
        running = true
        worker = thread(name = "video-receiver", priority = Thread.MAX_PRIORITY) {
            // Datagrams are capped at 1280 bytes by the host so nothing IP-fragments.
            val buffer = ByteArray(2048)
            val packet = DatagramPacket(buffer, buffer.size)
            while (running) {
                try {
                    s.receive(packet)
                    datagramsReceived++
                    handleDatagram(packet.data, packet.length)
                } catch (e: java.net.SocketTimeoutException) {
                    // Expected: lets the loop notice `running` going false.
                } catch (e: Exception) {
                    if (running) Log.w(TAG, "receive failed", e)
                }
            }
        }
    }

    fun stop() {
        running = false
        worker?.join(2000)
        worker = null
        socket?.close()
        socket = null
        pending.clear()
        keyframeFlags.clear()
        timestamps.clear()
    }

    private fun handleDatagram(data: ByteArray, length: Int) {
        if (length < Protocol.VIDEO_HEADER_SIZE) return

        val header = ByteBuffer.wrap(data, 0, Protocol.VIDEO_HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        if (header.int != Protocol.VIDEO_MAGIC) return

        val frameNumber = header.int
        val fragmentIndex = header.short.toInt() and 0xFFFF
        val fragmentCount = header.short.toInt() and 0xFFFF
        val timestamp = header.long
        val flags = header.get().toInt() and 0xFF
        header.get() // reserved
        val payloadSize = header.short.toInt() and 0xFFFF

        if (fragmentCount == 0 || fragmentIndex >= fragmentCount) return
        if (Protocol.VIDEO_HEADER_SIZE + payloadSize > length) return
        // Unsigned comparison: frame numbers are monotonic within a session.
        if (frameNumber != -1 && newestCompleted != -1 && frameNumber < newestCompleted) return

        val fragments = pending.getOrPut(frameNumber) {
            keyframeFlags[frameNumber] = (flags and Protocol.FLAG_KEYFRAME) != 0
            timestamps[frameNumber] = timestamp
            arrayOfNulls(fragmentCount)
        }
        if (fragments.size != fragmentCount) return

        fragments[fragmentIndex] = data.copyOfRange(
            Protocol.VIDEO_HEADER_SIZE,
            Protocol.VIDEO_HEADER_SIZE + payloadSize,
        )

        if (fragments.any { it == null }) return

        val total = fragments.sumOf { it!!.size }
        val frame = ByteArray(total)
        var offset = 0
        for (fragment in fragments) {
            fragment!!.copyInto(frame, offset)
            offset += fragment.size
        }

        val isKeyframe = keyframeFlags[frameNumber] ?: false
        val frameTimestamp = timestamps[frameNumber] ?: timestamp
        newestCompleted = frameNumber
        framesCompleted++

        // Everything older is now unshowable; count it and let it go.
        var lost = 0
        val stale = pending.keys.filter { it <= frameNumber }
        for (key in stale) {
            if (key != frameNumber) lost++
            pending.remove(key)
            keyframeFlags.remove(key)
            timestamps.remove(key)
        }
        if (lost > 0) {
            framesDropped += lost
            onFrameLost?.invoke()
        }

        onFrame(VideoFrame(frame, isKeyframe, frameTimestamp))
    }

    private companion object {
        const val TAG = "VideoReceiver"
    }
}
