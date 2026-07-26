package com.thorstream.client

import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.DataInputStream
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * The TCP control channel. Owns the socket and pumps inbound messages on a
 * background thread; callbacks arrive off the main thread.
 */
class ControlConnection(private val scope: CoroutineScope) {

    var onWindowList: ((List<WindowInfo>) -> Unit)? = null
    var onGameList: ((List<GameInfo>) -> Unit)? = null
    var onLaunchProgress: ((String) -> Unit)? = null
    var onStarted: ((SessionInfo) -> Unit)? = null
    var onError: ((String) -> Unit)? = null
    var onDisconnected: (() -> Unit)? = null
    var onPong: ((Long) -> Unit)? = null

    private var socket: Socket? = null
    private var readJob: Job? = null

    @Volatile
    private var connected = false

    val isConnected: Boolean get() = connected

    suspend fun connect(host: String, port: Int, timeoutMs: Int = 5000) {
        withContext(Dispatchers.IO) {
            val s = Socket()
            // Input latency matters far more than packing small writes together.
            s.connect(InetSocketAddress(host, port), timeoutMs)
            s.tcpNoDelay = true
            socket = s
            connected = true
            readJob = scope.launch(Dispatchers.IO) { readLoop(s) }
            send(Protocol.HELLO, ProtocolWriter().u16(Protocol.VERSION).str(android.os.Build.MODEL))
        }
    }

    fun requestStart(
        windowId: Long,
        width: Int,
        height: Int,
        fps: Int,
        bitrateKbps: Int,
        codec: Byte,
        udpPort: Int,
    ) {
        send(
            Protocol.START,
            ProtocolWriter()
                .u64(windowId)
                .u32(width)
                .u32(height)
                .u32(fps)
                .u32(bitrateKbps)
                .u8(codec.toInt())
                .u16(udpPort),
        )
    }

    /** Asks the host to launch a Playnite game and stream it once it appears. */
    fun requestLaunch(
        gameId: String,
        width: Int,
        height: Int,
        fps: Int,
        bitrateKbps: Int,
        codec: Byte,
        udpPort: Int,
    ) {
        send(
            Protocol.LAUNCH,
            ProtocolWriter()
                .str(gameId)
                .u32(width)
                .u32(height)
                .u32(fps)
                .u32(bitrateKbps)
                .u8(codec.toInt())
                .u16(udpPort),
        )
    }

    fun requestKeyframe() = send(Protocol.REQUEST_IDR, ProtocolWriter())

    fun stopSession() = send(Protocol.STOP, ProtocolWriter())

    fun sendGamepad(state: GamepadState) {
        send(
            Protocol.GAMEPAD,
            ProtocolWriter()
                .u16(state.buttons)
                .u8(state.leftTrigger)
                .u8(state.rightTrigger)
                .u16(state.leftStickX)
                .u16(state.leftStickY)
                .u16(state.rightStickX)
                .u16(state.rightStickY)
                .u32(state.sequence),
        )
    }

    fun ping(clientTimeMicros: Long) = send(Protocol.PING, ProtocolWriter().u64(clientTimeMicros))

    fun close() {
        connected = false
        readJob?.cancel()
        try {
            socket?.close()
        } catch (_: IOException) {
        }
        socket = null
    }

    private fun send(type: Byte, payload: ProtocolWriter) {
        if (!connected) return
        val body = payload.toByteArray()
        val frame = ByteBuffer.allocate(4 + 1 + body.size).order(ByteOrder.LITTLE_ENDIAN)
        frame.putInt(body.size + 1)
        frame.put(type)
        frame.put(body)

        scope.launch(Dispatchers.IO) {
            try {
                socket?.getOutputStream()?.apply {
                    write(frame.array())
                    flush()
                }
            } catch (e: IOException) {
                Log.w(TAG, "send failed", e)
                handleDisconnect()
            }
        }
    }

    private fun readLoop(s: Socket) {
        try {
            val input = DataInputStream(s.getInputStream().buffered())
            val lengthBytes = ByteArray(4)
            while (connected) {
                input.readFully(lengthBytes)
                val length = ByteBuffer.wrap(lengthBytes).order(ByteOrder.LITTLE_ENDIAN).int
                if (length <= 0 || length > MAX_MESSAGE) {
                    Log.w(TAG, "absurd message length $length; dropping connection")
                    break
                }
                val payload = ByteArray(length)
                input.readFully(payload)
                dispatch(payload[0], payload.copyOfRange(1, length))
            }
        } catch (e: Exception) {
            if (connected) Log.w(TAG, "read loop ended", e)
        }
        handleDisconnect()
    }

    private fun dispatch(type: Byte, payload: ByteArray) {
        val reader = ProtocolReader(payload)
        when (type) {
            Protocol.WINDOW_LIST -> {
                val count = reader.u16()
                val windows = ArrayList<WindowInfo>(count)
                repeat(count) {
                    windows.add(
                        WindowInfo(
                            id = reader.u64(),
                            width = reader.u32(),
                            height = reader.u32(),
                            process = reader.str(),
                            title = reader.str(),
                        )
                    )
                }
                onWindowList?.invoke(windows)
            }

            Protocol.STARTED -> {
                val width = reader.u32()
                val height = reader.u32()
                val codec = reader.u8().toByte()
                val headerLength = reader.u16()
                onStarted?.invoke(SessionInfo(width, height, codec, reader.bytes(headerLength)))
            }

            Protocol.GAME_LIST -> {
                val count = reader.u16()
                val games = ArrayList<GameInfo>(count)
                repeat(count) {
                    games.add(
                        GameInfo(
                            id = reader.str(),
                            name = reader.str(),
                            platform = reader.str(),
                            source = reader.str(),
                            installed = reader.u8() != 0,
                        )
                    )
                }
                onGameList?.invoke(games)
            }

            Protocol.LAUNCH_PROGRESS -> onLaunchProgress?.invoke(reader.str())

            Protocol.PONG -> onPong?.invoke(reader.u64())
            Protocol.ERROR -> onError?.invoke(reader.str())
            else -> Log.w(TAG, "unknown message type $type")
        }
    }

    private fun handleDisconnect() {
        if (!connected) return
        connected = false
        onDisconnected?.invoke()
    }

    private companion object {
        const val TAG = "ControlConnection"
        const val MAX_MESSAGE = 1 shl 20
    }
}
