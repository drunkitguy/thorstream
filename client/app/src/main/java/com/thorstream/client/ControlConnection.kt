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
import java.util.concurrent.LinkedBlockingQueue

/**
 * The TCP control channel. Owns the socket and pumps inbound messages on a
 * background thread; callbacks arrive off the main thread.
 *
 * Outbound messages go through one queue drained by one thread. They used to be
 * a coroutine each on Dispatchers.IO, which has 64 workers: two messages sent
 * microseconds apart went to different workers and raced to write(), so frames
 * could interleave mid-body and desync the host's length-prefixed parser, or
 * simply arrive out of order. Gamepad packets survived that - the next state
 * packet overwrites a stale one - but mouse buttons are edges, and a LEFTUP
 * overtaking its LEFTDOWN latches the button down on the user's PC with nothing
 * anywhere to let it go again.
 */
class ControlConnection(private val scope: CoroutineScope) {

    var onWindowList: ((List<WindowInfo>) -> Unit)? = null
    var onGameList: ((List<GameInfo>) -> Unit)? = null
    var onLaunchProgress: ((String) -> Unit)? = null
    var onCover: ((String, ByteArray?) -> Unit)? = null
    var onStarted: ((SessionInfo) -> Unit)? = null
    var onError: ((String) -> Unit)? = null
    var onDisconnected: (() -> Unit)? = null
    var onPong: ((Long) -> Unit)? = null

    private var socket: Socket? = null
    private var readJob: Job? = null

    // Unbounded on purpose: the one thing this queue must never do is drop a
    // message, because half of the ones it carries are button edges.
    private val outbound = LinkedBlockingQueue<ByteArray>()

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
            outbound.clear()
            connected = true
            // A plain thread rather than a coroutine on the caller's scope: this
            // is what lets a teardown message still go out after the activity
            // that queued it has cancelled its scope.
            Thread({ writeLoop(s) }, "control-writer").apply {
                isDaemon = true
                start()
            }
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

    /** Position is normalised 0..65535 across the host's desktop. */
    fun sendMouseMove(x: Int, y: Int) =
        send(Protocol.MOUSE_MOVE, ProtocolWriter().u16(x).u16(y))

    fun sendMouseButton(button: Int, pressed: Boolean) =
        send(Protocol.MOUSE_BUTTON, ProtocolWriter().u8(button).u8(if (pressed) 1 else 0))

    fun sendScroll(delta: Int) = send(Protocol.MOUSE_SCROLL, ProtocolWriter().u16(delta))

    fun sendKey(virtualKey: Int, pressed: Boolean) =
        send(Protocol.KEY, ProtocolWriter().u16(virtualKey).u8(if (pressed) 1 else 0))

    fun sendText(text: String) = send(Protocol.TEXT, ProtocolWriter().str(text))

    fun requestCover(gameId: String) =
        send(Protocol.COVER_REQUEST, ProtocolWriter().str(gameId))

    fun requestKeyframe() = send(Protocol.REQUEST_IDR, ProtocolWriter())

    fun stopSession() = send(Protocol.STOP, ProtocolWriter())

    /**
     * Sends STOP and closes, outliving the scope that queued it.
     *
     * The writer thread belongs to no lifecycle, so everything already queued
     * still goes out in order and the socket is closed behind it rather than
     * underneath it.
     *
     * [releaseButtons] are mouse buttons the caller is still holding down.
     * They are queued here rather than sent through the ordinary path because
     * this is the one moment that path can be shut down under a message, and a
     * button left down is not something the host, the protocol or Windows will
     * ever undo by itself: net_server.cpp injects button edges with no pairing
     * state, and onReleaseInput only resets the virtual gamepad.
     */
    fun stopSessionAndClose(releaseButtons: List<Int> = emptyList()) {
        if (socket == null) {
            // Nothing to send it down. A button held when the transport died
            // stays held: the release cannot be delivered from here, and the
            // host has no release-all to fall back on. Said out loud rather
            // than dropped quietly, because it is the one case where the
            // guarantee this method exists to make does not hold.
            if (releaseButtons.isNotEmpty()) {
                Log.w(TAG, "connection already down; $releaseButtons left held on the host")
            }
            return close()
        }
        // No new ordinary sends, but the queue is still drained.
        connected = false
        readJob?.cancel()
        socket = null

        for (button in releaseButtons) {
            outbound.add(
                frameFor(Protocol.MOUSE_BUTTON, ProtocolWriter().u8(button).u8(0))
            )
        }
        outbound.add(frameFor(Protocol.STOP, ProtocolWriter()))
        outbound.add(CLOSE_SENTINEL)
    }

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

    /**
     * Drops the connection at once, discarding anything still queued.
     *
     * Explicitly NOT the graceful flush [stopSessionAndClose] performs. This is
     * for a link that is already dead or is being abandoned, so the socket is
     * closed immediately - which is also what brings a read loop parked in
     * readFully straight out. The queue is emptied before the sentinel goes in
     * so the writer cannot pick up a frame and then fail it against the socket
     * that has just gone, which would only produce a misleading "send failed".
     */
    fun close() {
        connected = false
        readJob?.cancel()
        // The writer is parked on take(); the sentinel is the only thing that
        // gets it out. Closing the socket alone would leave it there for good.
        outbound.clear()
        outbound.add(CLOSE_SENTINEL)
        try {
            socket?.close()
        } catch (_: IOException) {
        }
        socket = null
    }

    private fun frameFor(type: Byte, payload: ProtocolWriter): ByteArray {
        val body = payload.toByteArray()
        val frame = ByteBuffer.allocate(4 + 1 + body.size).order(ByteOrder.LITTLE_ENDIAN)
        frame.putInt(body.size + 1)
        frame.put(type)
        frame.put(body)
        return frame.array()
    }

    /**
     * Queues a message. Ordering is the whole point: enqueueing is what fixes
     * the submission order, and exactly one thread ever writes to the socket.
     */
    private fun send(type: Byte, payload: ProtocolWriter) {
        if (!connected) return
        outbound.add(frameFor(type, payload))
    }

    private fun writeLoop(s: Socket) {
        try {
            val out = s.getOutputStream()
            while (true) {
                val frame = outbound.take()
                if (frame === CLOSE_SENTINEL) break
                out.write(frame)
                out.flush()
            }
        } catch (e: IOException) {
            Log.w(TAG, "send failed", e)
            handleDisconnect()
        } catch (_: InterruptedException) {
            Thread.currentThread().interrupt()
        } finally {
            // In a finally, not after the catches: an unchecked throw from a
            // frame builder or the stream would otherwise take the socket with
            // it, and this thread is the only thing that ever closes it.
            try {
                s.close()
            } catch (_: IOException) {
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

            Protocol.COVER_DATA -> {
                val gameId = reader.str()
                val length = reader.u32()
                // A zero-length reply means the host has no art for this game;
                // that is an answer, not a failure, and the tile should stop
                // waiting for one.
                val jpeg = if (length > 0) reader.bytes(length) else null
                onCover?.invoke(gameId, jpeg)
            }

            Protocol.PONG -> onPong?.invoke(reader.u64())
            Protocol.ERROR -> onError?.invoke(reader.str())
            else -> Log.w(TAG, "unknown message type $type")
        }
    }

    private fun handleDisconnect() {
        if (!connected) return
        connected = false
        // A link that died on its own still has to stop the writer, exactly as
        // the deliberate paths do. A thread parked on take() forever is a GC
        // root: it retains this connection, its callbacks, and whichever
        // activity installed them. Nothing else would ever collect it, because
        // LibraryActivity builds a fresh connection on onRestart and drops the
        // old reference without closing it - so a host restart or a wifi blip
        // followed by backgrounding would leak an activity every time.
        close()
        // After close, so a handler that inspects the connection is not told
        // about a disconnect while the object still looks half alive.
        onDisconnected?.invoke()
    }

    private companion object {
        const val TAG = "ControlConnection"
        const val MAX_MESSAGE = 1 shl 20

        // Compared by identity, so it can never collide with a real frame.
        val CLOSE_SENTINEL = ByteArray(0)
    }
}
