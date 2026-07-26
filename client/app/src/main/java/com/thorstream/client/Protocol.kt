package com.thorstream.client

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Wire format shared with the Windows host. Mirrors PROTOCOL.md and
 * host/src/protocol.h - if you change one, change all three.
 */
object Protocol {
    const val VERSION = 1
    const val DEFAULT_CONTROL_PORT = 47810

    const val VIDEO_MAGIC = 0x31565354 // 'TSV1'
    const val VIDEO_HEADER_SIZE = 24
    const val FLAG_KEYFRAME: Int = 0x01

    // Client -> host
    const val HELLO: Byte = 0x01
    const val START: Byte = 0x03
    const val STOP: Byte = 0x05
    const val REQUEST_IDR: Byte = 0x06
    const val GAMEPAD: Byte = 0x07
    const val PING: Byte = 0x08

    // Host -> client
    const val WINDOW_LIST: Byte = 0x02
    const val STARTED: Byte = 0x04
    const val PONG: Byte = 0x09
    const val ERROR: Byte = 0x0A

    const val CODEC_H264: Byte = 0
    const val CODEC_HEVC: Byte = 1
}

/** XInput button bits; the host injects these straight into a virtual pad. */
object GamepadButton {
    const val DPAD_UP = 1 shl 0
    const val DPAD_DOWN = 1 shl 1
    const val DPAD_LEFT = 1 shl 2
    const val DPAD_RIGHT = 1 shl 3
    const val START = 1 shl 4
    const val BACK = 1 shl 5
    const val LEFT_THUMB = 1 shl 6
    const val RIGHT_THUMB = 1 shl 7
    const val LEFT_SHOULDER = 1 shl 8
    const val RIGHT_SHOULDER = 1 shl 9
    const val A = 1 shl 12
    const val B = 1 shl 13
    const val X = 1 shl 14
    const val Y = 1 shl 15
}

data class WindowInfo(
    val id: Long,
    val width: Int,
    val height: Int,
    val process: String,
    val title: String,
)

data class SessionInfo(
    val width: Int,
    val height: Int,
    val codec: Byte,
    val sequenceHeader: ByteArray,
) {
    // Data classes compare arrays by reference by default, which is a trap.
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is SessionInfo) return false
        return width == other.width && height == other.height && codec == other.codec &&
            sequenceHeader.contentEquals(other.sequenceHeader)
    }

    override fun hashCode(): Int =
        (((width * 31 + height) * 31 + codec) * 31) + sequenceHeader.contentHashCode()
}

class ProtocolWriter {
    private val out = ByteArrayOutputStream()

    fun u8(value: Int) = apply { out.write(value and 0xFF) }

    fun u16(value: Int) = apply {
        out.write(value and 0xFF)
        out.write((value ushr 8) and 0xFF)
    }

    fun u32(value: Int) = apply {
        for (shift in 0..24 step 8) out.write((value ushr shift) and 0xFF)
    }

    fun u64(value: Long) = apply {
        for (shift in 0..56 step 8) out.write(((value ushr shift) and 0xFF).toInt())
    }

    fun str(value: String) = apply {
        val bytes = value.toByteArray(Charsets.UTF_8)
        u16(bytes.size)
        out.write(bytes)
    }

    fun raw(bytes: ByteArray) = apply { out.write(bytes) }

    fun toByteArray(): ByteArray = out.toByteArray()
}

class ProtocolReader(private val data: ByteArray) {
    private val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)

    fun u8(): Int = buffer.get().toInt() and 0xFF
    fun u16(): Int = buffer.short.toInt() and 0xFFFF
    fun u32(): Int = buffer.int
    fun u64(): Long = buffer.long

    fun str(): String {
        val length = u16()
        val bytes = ByteArray(length)
        buffer.get(bytes)
        return String(bytes, Charsets.UTF_8)
    }

    fun bytes(count: Int): ByteArray = ByteArray(count).also { buffer.get(it) }

    fun remaining(): Int = buffer.remaining()
}
