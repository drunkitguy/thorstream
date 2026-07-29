package com.thorstream.client

import android.annotation.SuppressLint
import android.os.Build
import android.text.Editable
import android.text.TextWatcher
import android.view.inputmethod.InputMethodManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.DisplayMetrics
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.View
import android.view.WindowManager
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.thorstream.client.databinding.ActivityStreamBinding
import kotlinx.coroutines.launch
import java.util.Locale
import kotlin.math.abs
import kotlin.math.exp
import kotlin.math.min
import kotlin.math.roundToInt

/** The actual streaming screen: video in, gamepad out. */
class StreamActivity : AppCompatActivity() {

    private lateinit var binding: ActivityStreamBinding

    private var control: ControlConnection? = null
    private var receiver: VideoReceiver? = null
    private var decoder: VideoDecoder? = null
    private lateinit var gamepad: GamepadTracker
    private lateinit var touch: TouchController
    private var touchEnabled = true
    private var touchMappable = false

    private val handler = Handler(Looper.getMainLooper())
    private var surfaceReady = false
    private var sessionStarted = false
    private var lastIdrRequest = 0L
    private var latencyMillis = -1L

    private var lastStatsTime = 0L
    private var lastStatsFrames = 0L
    private var lastStatsBytes = 0L
    // The codec this connection asked for, snapshotted in connect() and carried
    // through startDecoder - the same byte the decoder was configured with. Not
    // codecChain[codecIndex], which a fallback advances before the replacement
    // decoder exists, and not session.codec, which VideoDecoder documents as not
    // trustworthy. The stats line must never name a codec that is not on screen.
    private var activeCodecName = ""

    private var keyboardVisible = false
    private var selectHeld = false
    private var startHeld = false
    private var comboConsumed = false

    private var loadStage = LoadStage.CONNECTING
    private var stageEnteredAt = 0L
    private var loadingStartedAt = 0L
    private var loadingNote = ""
    private var loadingVisible = true
    private var loadingErrorShown = false
    private var loadingErrorRank = ERROR_RANK_NONE
    private var stallReported = false
    private var pendingSession: SessionInfo? = null
    private var pendingCodec: Byte = Protocol.CODEC_H264

    // The codecs to try, best first, and where we are in that list. Fixed in
    // onCreate: the chain is a property of this device and this user's setting,
    // and it must not change underneath a retry.
    private var codecChain: List<Byte> = listOf(Protocol.CODEC_H264)
    private var codecIndex = 0
    private var fallbackInFlight = false

    @SuppressLint("ClickableViewAccessibility")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityStreamBinding.inflate(layoutInflater)
        setContentView(binding.root)

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        goImmersive()

        val title: String = intent.getStringExtra(EXTRA_TITLE) ?: "streaming"
        binding.title.text = title
        binding.loadingTitle.text = title
        startLoading()

        gamepad = GamepadTracker { state -> control?.sendGamepad(state) }

        // Read from the preferences rather than an intent extra: a front end
        // such as Cocoon reaches this screen without passing through the picker
        // that would otherwise carry the flag. The codec preference is read here
        // for the same reason.
        val prefs = getSharedPreferences(MainActivity.PREFS, MODE_PRIVATE)
        touchEnabled = prefs.getBoolean(MainActivity.KEY_TOUCH, true)
        codecChain = CodecSupport.chainFor(prefs.getInt(MainActivity.KEY_CODEC, CodecSupport.PREF_AV1))
        Log.i(TAG, "codec chain: ${codecChain.joinToString { Protocol.codecName(it) }}")

        touch = TouchController(
            // The overlay is opaque and full screen but not clickable, so its
            // touches reach the surface behind it. Nothing the user cannot see
            // should be able to move the pointer on the PC.
            enabled = { touchEnabled && touchMappable && !loadingVisible },
            onMove = { x, y -> control?.sendMouseMove(x, y) },
            onButton = { button, pressed -> control?.sendMouseButton(button, pressed) },
            onScroll = { delta -> control?.sendScroll(delta) },
        )
        // Only the video surface, and only through dispatchTouchEvent: a
        // touchscreen never reaches dispatchGenericMotionEvent, so this and the
        // gamepad's motion handling cannot swallow one another. The keyboard
        // sink keeps its own touches, and the IME is a separate window.
        binding.surface.setOnTouchListener { view, event -> touch.onTouch(view, event) }

        setUpKeyboardSink()

        binding.surface.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                surfaceReady = true
                connect()
                // STARTED can land while the surface is gone - Home during a
                // three-minute launch - and connect() will not retry because the
                // control connection is still up. Without this the session is
                // stranded with no decoder and nothing to show for it.
                // The codec goes with it: the session it was deferred from is
                // the one that must be decoded, whatever the chain has moved on
                // to since.
                pendingSession?.let { session -> startDecoder(session, pendingCodec) }
            }

            override fun surfaceChanged(holder: SurfaceHolder, f: Int, w: Int, h: Int) = Unit

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                surfaceReady = false
            }
        })
    }

    private fun connect() {
        if (control != null) return

        val host = intent.getStringExtra(EXTRA_HOST) ?: return finish()
        val windowId = intent.getLongExtra(EXTRA_WINDOW_ID, 0)
        val bitrate = intent.getIntExtra(EXTRA_BITRATE, MainActivity.DEFAULT_BITRATE_KBPS)
        val fps = intent.getIntExtra(EXTRA_FPS, 60)

        val videoReceiver = VideoReceiver { frame -> decoder?.submit(frame) }
        val udpPort = videoReceiver.bind()
        videoReceiver.onFrameLost = { requestKeyframeThrottled() }
        videoReceiver.start()
        receiver = videoReceiver

        val connection = ControlConnection(lifecycleScope)
        control = connection
        val codec = codecChain[codecIndex]

        connection.onStarted = { session ->
            runOnUiThread { if (isCurrent(connection)) startDecoder(session, codec) }
        }
        connection.onLaunchProgress = { message ->
            runOnUiThread {
                if (isCurrent(connection)) {
                    setStatus(message)
                    showLaunchProgress(message)
                }
            }
        }
        connection.onError = { message ->
            runOnUiThread {
                if (!isCurrent(connection)) return@runOnUiThread
                setStatus("Host: $message")
                // The host never substitutes a codec silently: a GPU that cannot
                // encode what was asked for says so and stops. That is a
                // recoverable answer, not a dead end, as long as something else
                // is left in the chain.
                if (looksLikeCodecRefusal(message, codec)) {
                    fallBack("Your PC refused ${Protocol.codecName(codec)}", message)
                } else {
                    showLoadingError("The PC could not start the game", message, ERROR_RANK_REPORTED)
                }
            }
        }
        connection.onDisconnected = {
            runOnUiThread {
                if (!isCurrent(connection)) return@runOnUiThread
                // A codec refusal is often followed by the host dropping the
                // channel. The retry is already scheduled and this would only
                // paint a complaint over the explanation on its way out.
                if (fallbackInFlight) return@runOnUiThread
                setStatus("Disconnected from host.")
                // Fallout rank: a single failed write drops the channel, and the
                // real reason usually arrives right behind it.
                showLoadingError(
                    "Lost the connection to your PC",
                    "The control channel closed. If the game is still starting it may recover on its own.",
                    ERROR_RANK_FALLOUT,
                )
            }
        }
        connection.onPong = { sentMicros ->
            // Measured on the read thread, applied on the main one: guarding it
            // like the rest keeps a dropped connection's last pong from
            // reporting a round trip for the session that replaced it, and
            // leaves latencyMillis owned by the thread that reads it.
            val trip = (nowMicros() - sentMicros) / 1000
            runOnUiThread { if (isCurrent(connection)) latencyMillis = trip }
        }

        // Logged unconditionally: if the built-in controls are not enumerated as
        // a game controller at all, no amount of key mapping will help, and that
        // is worth knowing before blaming the network.
        val pads = describeControllers()
        Log.i(TAG, "input devices: $pads")
        setStatus(if (pads == NO_CONTROLLER) "$NO_CONTROLLER — connecting..." else "Connecting...")
        lifecycleScope.launch {
            try {
                connection.connect(host, Protocol.DEFAULT_CONTROL_PORT)
                val gameId = intent.getStringExtra(EXTRA_GAME_ID)
                // Our panel is the ceiling: streaming a 4K window to a 1080p
                // screen costs 4x the bandwidth and decode time for nothing the
                // eye can resolve. The host fits the window inside this box and
                // keeps its aspect ratio.
                val (maxWidth, maxHeight) = displaySize()
                if (gameId != null) {
                    // The host launches it, waits for its window, and only then
                    // replies with STARTED - which can take minutes for a large
                    // game, hence the progress messages.
                    connection.requestLaunch(
                        gameId = gameId,
                        width = maxWidth,
                        height = maxHeight,
                        fps = fps,
                        bitrateKbps = bitrate,
                        codec = codec,
                        udpPort = udpPort,
                    )
                } else {
                    connection.requestStart(
                        windowId = windowId,
                        width = maxWidth,
                        height = maxHeight,
                        fps = fps,
                        bitrateKbps = bitrate,
                        codec = codec,
                        udpPort = udpPort,
                    )
                    // Nothing is being launched, so the host sends no progress
                    // messages on this path and the bar would sit near zero
                    // until STARTED.
                    runOnUiThread {
                        enterStage(
                            LoadStage.STARTING_STREAM,
                            "Starting the stream",
                            noteFor(LoadStage.STARTING_STREAM),
                        )
                    }
                }
            } catch (e: Exception) {
                runOnUiThread {
                    setStatus("Could not connect: ${e.message}")
                    showLoadingError(
                        "Could not reach your PC",
                        e.message ?: "unknown error",
                        ERROR_RANK_REPORTED,
                    )
                }
            }
        }
    }

    /**
     * Brings up the decoder for a started session.
     *
     * Kept separate from onStarted because the surface can be gone when STARTED
     * lands - the user pressed Home during a long launch - and a session dropped
     * there is never offered again by the host. Holding it in [pendingSession]
     * lets surfaceCreated finish the job instead of hanging on a dead screen.
     */
    private fun startDecoder(session: SessionInfo, codec: Byte) {
        // STARTED can land after teardown: runOnUiThread still delivers once the
        // activity is destroyed. Configuring a codec against a dead surface
        // leaks it, and the stats ticker it would start reposts itself onto an
        // already-flushed handler, holding the activity for good.
        if (isFinishing || isDestroyed) return
        if (decoder != null) return
        if (!surfaceReady) {
            pendingSession = session
            pendingCodec = codec
            loadingNote = "Waiting for the display to come back"
            return
        }
        pendingSession = null

        fitSurfaceToVideo(session.width, session.height)
        touch.setVideoSize(session.width, session.height)
        touchMappable = pictureLooksLikeTheDesktop(session)

        val codecName = Protocol.codecName(codec)
        setStatus("Decoding ${session.width}x${session.height} · $codecName")
        val videoDecoder = VideoDecoder(binding.surface.holder.surface)
        videoDecoder.onError = { message ->
            runOnUiThread {
                // Identity-guarded like the connection callbacks: a decoder that
                // a fallback has already released can still deliver one last
                // error, and it must not paint a complaint over the attempt that
                // replaced it - which would undo the very clearLoadingError that
                // started the retry.
                if (videoDecoder !== decoder) {
                    Log.w(TAG, "error from a released decoder, ignored: $message")
                    return@runOnUiThread
                }
                setStatus("Decoder error: $message")
                // A codec that dies before it has shown anything never worked in
                // the first place, whatever it advertised - some decoders accept
                // a format at configure() and only fail on the first buffer. That
                // is the same failure as a refused configure and gets the same
                // answer. After a picture has been on screen it is a fault in a
                // working session, and dropping to a worse codec would not fix it.
                if (!videoDecoder.hasRenderedFrame) {
                    fallBack("$codecName could not decode this stream", message)
                } else {
                    showLoadingError("The video decoder failed", message, ERROR_RANK_REPORTED)
                }
            }
        }
        try {
            videoDecoder.start(session, codec)
            decoder = videoDecoder
            sessionStarted = true
            // Set only once the codec has been accepted, so a refused configure
            // cannot leave the stats line naming a decoder that never ran.
            activeCodecName = codecName
            // STARTED is not a picture. This is the stage the complaint was
            // about: the host is streaming but nothing has decoded yet, so the
            // overlay stays until something has.
            enterStage(
                LoadStage.DECODING,
                "Waiting for the first frame",
                "Decoding ${session.width}x${session.height} · $codecName",
            )
            // Anything the host sent before this moment went nowhere, including
            // the keyframe the stream opened with. Ask for a fresh one rather
            // than waiting for a decoder that will never produce a picture.
            control?.requestKeyframe()
            lastIdrRequest = System.currentTimeMillis()
            startStatsTicker()
        } catch (e: Exception) {
            // configure()/start() refusing the format is exactly what a device
            // that lied about its decoder looks like, so this is a fallback
            // trigger rather than a message. fallBack settles for an error when
            // there is nothing left below.
            Log.e(TAG, "decoder failed to start", e)
            setStatus("Could not start the decoder: ${e.message}")
            fallBack("This device could not start a $codecName decoder", e.message ?: "unknown error")
        }
    }

    // ---- codec fallback ------------------------------------------------------

    /**
     * Whether an ERROR from the host is a refusal of THIS codec.
     *
     * The host has exactly two ways of saying that: an unrecognised codec byte
     * ("this host does not know codec 2 ..."), and a GPU that knows the codec
     * but cannot encode it ("NVENC: this GPU cannot encode AV1 ..."). The second
     * names the codec deliberately, and that name is the whole test.
     *
     * Deliberately NOT the "NVENC: " prefix or the word "encoder". The host puts
     * that prefix on every encoder-creation failure there is: a busy encoder
     * (GeForce cards cap concurrent NVENC sessions), a driver too old for the
     * NVENC API, out of memory, bad dimensions. None of those are fixed by
     * asking for a different codec, and on the launch path every retry relaunches
     * the game - so treating them as codec refusals would spend the whole chain,
     * cost three launches, and finish on a diagnosis that was wrong from the
     * start.
     */
    private fun looksLikeCodecRefusal(message: String, codec: Byte): Boolean {
        val text = message.lowercase(Locale.US)
        if ("does not know codec" in text) return true
        if ("cannot encode" !in text) return false
        return spellingsOf(codec).any { it in text }
    }

    /** The host writes one of these; the others are here so it may change its mind. */
    private fun spellingsOf(codec: Byte): List<String> = when (codec) {
        Protocol.CODEC_AV1 -> listOf("av1", "av01")
        Protocol.CODEC_HEVC -> listOf("hevc", "h.265", "h265")
        else -> listOf("h.264", "h264", "avc")
    }

    /**
     * Drops to the next codec down and starts the session again.
     *
     * Bounded by construction: the chain is fixed at onCreate and the index only
     * ever moves forwards, so the worst case is AV1, then HEVC, then H.264, then
     * an honest error. Nothing here can loop.
     */
    private fun fallBack(headline: String, detail: String) {
        if (isFinishing || isDestroyed) return
        // A refused codec can produce several complaints at once - the host's
        // ERROR, then the disconnect behind it - and they must not each consume
        // a codec.
        if (fallbackInFlight) return
        // Only ever before the first picture. Once one has been on screen the
        // codec demonstrably works on both ends, the overlay that would explain
        // a retry is gone, and tearing a live session down for a worse codec
        // would be a regression rather than a rescue.
        if (decoder?.hasRenderedFrame == true) {
            Log.w(TAG, "not falling back once a picture has shown: $headline ($detail)")
            return
        }

        val failed = Protocol.codecName(codecChain[codecIndex])
        if (codecIndex + 1 >= codecChain.size) {
            showLoadingError(
                headline,
                "$detail\n\nThere is no other codec left to try.",
                ERROR_RANK_DIAGNOSED,
            )
            return
        }

        fallbackInFlight = true
        val next = Protocol.codecName(codecChain[codecIndex + 1])
        Log.w(TAG, "falling back from $failed to $next: $detail")
        setStatus("$failed failed — retrying with $next")
        restartLoadingFor("Retrying with $next", "$failed did not work: $detail")

        // Posted rather than done here: the overlay repaints with the reason
        // before the teardown, which joins the receiver and decoder threads and
        // can hold the main thread for a moment.
        handler.postDelayed({
            fallbackInFlight = false
            if (isFinishing || isDestroyed) return@postDelayed
            // Checked again, not only at the top of fallBack. The delay is a
            // scheduling gap, and a first frame landing inside it is the
            // ordinary slow start recovering: a keyframe is re-requested every
            // second and the one that finally arrives renders in milliseconds,
            // so the 8-second stall trigger in particular fires right next to
            // the picture it was complaining about. Tearing that down would
            // destroy a working session - and by then the loading ticker has
            // hidden the overlay, which is what B and Back need to leave the
            // screen and what keeps a live touch gesture off the PC.
            if (decoder?.hasRenderedFrame == true) {
                Log.i(TAG, "picture arrived during the fallback delay; staying on $failed")
                setStatus("$failed recovered")
                return@postDelayed
            }
            // Only now: a fallback that was abandoned must not have spent a
            // codec, or a later real failure would skip one.
            codecIndex++
            teardownSession()
            connect()
        }, FALLBACK_DELAY_MS)
    }

    /** Everything a session owns, released, with the screen left standing. */
    private fun teardownSession() {
        // Before the connection goes: the stats ticker reposts itself from here
        // and would otherwise tick against a half-released session.
        sessionStarted = false
        handler.removeCallbacks(statsTicker)
        pendingSession = null
        touchMappable = false
        latencyMillis = -1
        lastStatsFrames = 0
        activeCodecName = ""
        // Exactly what onDestroy does, and for the same reason: a button can
        // genuinely be down here. A picture that reached the screen retires the
        // overlay, which is what arms touch, so a retry can land mid-drag.
        // close() would discard the release - it empties the queue before the
        // sentinel - and nothing in the host, the protocol or Windows ever
        // undoes a latched button. stopSessionAndClose gets it out on the
        // writer thread, which outlives this teardown.
        val heldButtons = if (::touch.isInitialized) touch.abandon() else emptyList()
        control?.stopSessionAndClose(heldButtons)
        control = null
        receiver?.stop()
        receiver = null
        decoder?.stop()
        decoder = null
    }

    /** True while [connection] is the one this screen is currently using. */
    private fun isCurrent(connection: ControlConnection): Boolean = connection === control

    /**
     * Winds the overlay back for another attempt.
     *
     * Deliberately not enterStage, which refuses to move backwards: that rule
     * exists so late progress messages cannot drag the bar back, but a retry
     * genuinely does start the sequence again, and a bar parked at 90% through a
     * second connect would be the lie it was meant to prevent. The elapsed clock
     * is not reset - the user has been waiting since the first attempt.
     */
    private fun restartLoadingFor(label: String, note: String) {
        clearLoadingError()
        stallReported = false
        loadStage = LoadStage.CONNECTING
        stageEnteredAt = SystemClock.elapsedRealtime()
        binding.loadingBar.progress = 0
        loadingNote = note
        binding.loadingStage.text = label

        // The overlay is brought back in full, not merely redrawn. hideLoading
        // both clears loadingVisible and removes the ticker, and the ticker is
        // the only thing that can ever retire the overlay again - so setting the
        // text without these two lines leaves an invisible overlay that cannot
        // come back, while dispatchKeyEvent still gates B and Back on
        // loadingVisible. That is a screen with no picture and no way off it.
        loadingVisible = true
        binding.loadingOverlay.visibility = View.VISIBLE
        handler.removeCallbacks(loadingTicker)
        handler.post(loadingTicker)
    }

    /** What Android thinks is attached, for when no button does anything at all. */
    private fun describeControllers(): String {
        // toList() first: getDeviceIds() returns a primitive IntArray, which has
        // no mapNotNull - that only exists on Array<T> and Iterable.
        val ids: List<Int> = InputDevice.getDeviceIds().toList()
        val pads: List<InputDevice> = ids
            .mapNotNull { id -> InputDevice.getDevice(id) }
            .filter { device -> device.isGameController() }
        if (pads.isEmpty()) return NO_CONTROLLER
        return pads.joinToString(", ") { device ->
            "${device.name} (sources 0x${Integer.toHexString(device.sources)})"
        }
    }

    /**
     * Whether a touch can be mapped onto the host's desktop at all.
     *
     * The host injects with MOUSEEVENTF_VIRTUALDESK - 0..65535 across the whole
     * virtual desktop - so the mapping only means anything while the picture IS
     * the whole virtual desktop. The host arranges that on the launch path: a
     * virtual display at the size we asked for, the physical displays detached,
     * and the game's client area placed over the lot.
     *
     * The only evidence of that reaching this client is the encoded size, and it
     * is ONE-WAY evidence. A size smaller than we asked for proves the capture
     * is not the whole display - the window refused to resize, or the virtual
     * display was never created and host/src/session.cpp fell through to
     * "streaming the window as-is". A matching size proves nothing in the other
     * direction: a bordered window's client area is the right size and still
     * sits a caption height down the desktop, and a DetachPhysicalDisplays that
     * failed leaves the virtual desktop spanning monitors that are not in the
     * picture at all. Neither is visible from here.
     *
     * ASSUMPTION, deliberately left standing: when the sizes agree, the picture
     * covers the desktop. The real fix is host-side - STARTED carrying the
     * captured client rect's desktop origin and size, normalised through that
     * instead - and is queued separately. This refuses the cases it can see
     * rather than pretending to a test it cannot make.
     */
    private fun pictureLooksLikeTheDesktop(session: SessionInfo): Boolean {
        val (panelWidth, panelHeight) = displaySize()
        // FitPreservingAspect rounds down to an even size, so exact equality is
        // the wrong test by a pixel.
        val mapped = abs(session.width - panelWidth) <= SIZE_SLACK &&
            abs(session.height - panelHeight) <= SIZE_SLACK
        if (!mapped) {
            Log.w(
                TAG,
                "touch disabled: encoded ${session.width}x${session.height} is not the " +
                    "requested ${panelWidth}x${panelHeight}, so the picture is not the " +
                    "whole desktop and a position on it cannot be placed on the host",
            )
        }
        return mapped
    }

    /**
     * Gives the SurfaceView the stream's aspect ratio, letterboxing the rest.
     *
     * A Surface is stretched to whatever size the view has, so a stream that is
     * not the panel's shape is otherwise distorted - and, worse for touch, a
     * stretched picture has no bars but also no honest relationship between a
     * point on the panel and a point on the PC. The layout already centres the
     * surface, so shrinking it puts the difference into black bars that
     * TouchController then knows to ignore.
     *
     * Usually a no-op: the host builds its virtual display at the size we asked
     * for, so the encode matches the panel exactly. Left alone in that case
     * rather than resized to the same value, because a SurfaceView that
     * MediaCodec is already rendering into is not free to re-lay-out.
     */
    private fun fitSurfaceToVideo(videoWidth: Int, videoHeight: Int) {
        if (videoWidth <= 0 || videoHeight <= 0) return
        // displaySize(), not the root view's bounds: that is the box the host
        // was asked to fit the picture into, and measuring the fit against
        // anything else invents bars out of the disagreement.
        val (panelWidth, panelHeight) = displaySize()
        if (panelWidth <= 0 || panelHeight <= 0) return

        val scale = min(
            panelWidth.toFloat() / videoWidth,
            panelHeight.toFloat() / videoHeight,
        )
        val fittedWidth = (videoWidth * scale).roundToInt()
        val fittedHeight = (videoHeight * scale).roundToInt()
        // A pixel of rounding is not worth a relayout, and not worth a bar.
        if (abs(fittedWidth - panelWidth) <= 1 && abs(fittedHeight - panelHeight) <= 1) return

        binding.surface.layoutParams = binding.surface.layoutParams.apply {
            width = fittedWidth
            height = fittedHeight
        }
    }

    /** The panel's real pixel size, ignoring any window insets. */
    private fun displaySize(): Pair<Int, Int> {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val bounds = windowManager.currentWindowMetrics.bounds
            return bounds.width() to bounds.height()
        }
        @Suppress("DEPRECATION")
        val metrics = DisplayMetrics().also { windowManager.defaultDisplay.getRealMetrics(it) }
        return metrics.widthPixels to metrics.heightPixels
    }

    private fun requestKeyframeThrottled() {
        // A burst of loss must not become a burst of keyframes; that would make
        // congestion worse exactly when the link is already struggling.
        val now = System.currentTimeMillis()
        if (now - lastIdrRequest < IDR_THROTTLE_MS) return
        lastIdrRequest = now
        control?.requestKeyframe()
    }

    /**
     * A field rather than an anonymous Runnable posted per session, so a codec
     * fallback can cancel it. Left in flight, the previous session's ticker
     * reposts itself the moment the retry sets sessionStarted again, and the
     * screen ends up with two of them pinging forever.
     */
    private val statsTicker = object : Runnable {
        override fun run() {
            if (!sessionStarted) return
            updateStats()
            control?.ping(nowMicros())
            gamepad.heartbeat()
            // A keyframe request can itself be lost, and until one lands the
            // screen stays black with no other symptom. Keep asking.
            // hasRenderedFrame rather than hasKeyframe: a keyframe accepted
            // for queueing can still be dropped on the way to the codec, and
            // then nobody would ever ask for another one.
            decoder?.let { if (!it.hasRenderedFrame) requestKeyframeThrottled() }
            handler.postDelayed(this, 1000)
        }
    }

    private fun startStatsTicker() {
        lastStatsTime = System.currentTimeMillis()
        handler.removeCallbacks(statsTicker)
        handler.post(statsTicker)
    }

    private fun updateStats() {
        val videoReceiver = receiver ?: return
        val now = System.currentTimeMillis()
        val elapsed = (now - lastStatsTime).coerceAtLeast(1) / 1000.0

        val frames = videoReceiver.framesCompleted
        val fps = (frames - lastStatsFrames) / elapsed
        lastStatsFrames = frames
        lastStatsTime = now

        // Distinguish "no data" from "data arriving but undecodable". Both look
        // like a black screen, and they have completely different causes.
        val activeDecoder = decoder
        if (activeDecoder != null && !activeDecoder.hasRenderedFrame) {
            val diagnosis = if (frames == 0L) "no video received — check the network"
                else "nothing decoded yet · $frames frames received"
            setStatus(diagnosis)
            // Until the first frame lands the overlay is what the user is
            // actually looking at, so the diagnosis has to go there too.
            if (loadingVisible) loadingNote = diagnosis
            return
        }

        val latency = if (latencyMillis >= 0) "${latencyMillis}ms rtt" else "…"
        // The codec rides along with the numbers rather than being announced
        // once at startup: this ticker repaints every second and used to wipe
        // the "Decoding WxH · <codec>" line off the screen before it could be
        // read. Prefixed, and only the short name - "AV1 · " is four characters
        // in front of a line that already fits, so nothing is pushed off the
        // edge of a handheld's overlay.
        val codec = if (activeCodecName.isEmpty()) "" else "$activeCodecName · "
        setStatus("%s%.0f fps · %s · %d dropped".format(codec, fps, latency,
            videoReceiver.framesDropped))
    }

    private fun setStatus(message: String) {
        binding.status.text = message
    }

    // ---- loading overlay -----------------------------------------------------

    /**
     * One slice of the progress bar per stage the host reports. The host says
     * which stage it is in but never how far through it is, so entering a stage
     * jumps to its start and the bar then eases towards its end over roughly
     * [creepSeconds]. The easing is asymptotic on purpose: a launch that takes
     * three minutes still moves the bar rather than parking it on a dead value,
     * and it can never run into the next stage's slice.
     */
    private enum class LoadStage(val start: Int, val end: Int, val creepSeconds: Float) {
        CONNECTING(0, 50, 4f),
        UNLOCKING(60, 120, 12f),
        PREPARING_DISPLAY(130, 200, 6f),
        LAUNCHING(210, 280, 8f),
        WAITING(300, 700, 75f),
        STARTING_STREAM(720, 800, 4f),
        DECODING(820, 980, 15f),
    }

    private fun startLoading() {
        loadingStartedAt = SystemClock.elapsedRealtime()
        // enterStage only restamps this on a change of stage, and we start off
        // already in CONNECTING.
        stageEnteredAt = loadingStartedAt
        enterStage(LoadStage.CONNECTING, "Connecting to your PC", noteFor(LoadStage.CONNECTING))
        handler.post(loadingTicker)
    }

    private val loadingTicker = object : Runnable {
        override fun run() {
            if (!loadingVisible) return
            // Deliberately not stopped by an error: this is the only path that
            // can ever hide the overlay, and MediaCodec reports plenty of
            // exceptions it goes on to recover from. If a picture arrives after
            // an error, the picture wins.
            //
            // hasRenderedFrame, not hasKeyframe: the latter is set when a
            // keyframe is accepted for queueing, and that frame can still be
            // dropped before the codec sees it.
            if (decoder?.hasRenderedFrame == true) {
                hideLoading()
                return
            }
            drawLoading()
            handler.postDelayed(this, LOADING_TICK_MS)
        }
    }

    /** Maps the host's progress strings (host/src/session.cpp) onto bar stages. */
    private fun showLaunchProgress(message: String) {
        val label: String = message.trimEnd('.', '…', ' ').ifEmpty { message }
        val stage: LoadStage? = when {
            message.startsWith("Unlocking", ignoreCase = true) -> LoadStage.UNLOCKING
            message.startsWith("Preparing", ignoreCase = true) -> LoadStage.PREPARING_DISPLAY
            message.startsWith("Launching", ignoreCase = true) -> LoadStage.LAUNCHING
            message.startsWith("Waiting", ignoreCase = true) -> LoadStage.WAITING
            message.startsWith("Starting the stream", ignoreCase = true) -> LoadStage.STARTING_STREAM
            else -> null
        }
        // A message we do not recognise is still news worth showing; it just
        // must not guess at a position on the bar.
        if (stage == null) {
            binding.loadingStage.text = label
            return
        }
        enterStage(stage, label, noteFor(stage))
    }

    private fun noteFor(stage: LoadStage): String = when (stage) {
        LoadStage.CONNECTING -> "Opening the control channel"
        LoadStage.UNLOCKING -> "Signing in on the PC"
        LoadStage.PREPARING_DISPLAY -> "Switching the PC to the streaming display"
        LoadStage.LAUNCHING -> "Handing the game to its launcher"
        LoadStage.WAITING -> "Launcher updates and shader compilation can take several minutes"
        LoadStage.STARTING_STREAM -> "Capturing the game window"
        LoadStage.DECODING -> "Waiting for the first video frame"
    }

    private fun enterStage(stage: LoadStage, label: String, note: String) {
        // Progress messages can repeat or arrive late; the bar must never walk
        // backwards in front of the user.
        if (stage.ordinal < loadStage.ordinal) return
        if (stage != loadStage) {
            loadStage = stage
            stageEnteredAt = SystemClock.elapsedRealtime()
            // Reaching a new stage is proof the previous complaint is stale.
            clearLoadingError()
        }
        loadingNote = note
        binding.loadingStage.text = label
        drawLoading()
    }

    private fun drawLoading() {
        val now = SystemClock.elapsedRealtime()
        val inStage = (now - stageEnteredAt) / 1000f

        // A bar that keeps creeping under an error message is a lie about what
        // is happening, so once something is wrong it holds where it is.
        if (!loadingErrorShown) {
            val eased = 1f - exp(-inStage / loadStage.creepSeconds)
            val value = loadStage.start + ((loadStage.end - loadStage.start) * eased).toInt()
            if (value > binding.loadingBar.progress) binding.loadingBar.setProgress(value, true)
        }

        // The clock is the honest part: however slowly the bar creeps, this
        // proves the handheld is still waiting on something rather than hung.
        binding.loadingDetail.text = "${elapsedText(now - loadingStartedAt)} · $loadingNote"

        // The host thinks it is streaming and nothing has arrived. Say so: this
        // is the state a blocked video port lands in, and it will never end on
        // its own.
        if (loadStage == LoadStage.DECODING && !stallReported) {
            // Reaching here means the ticker has not seen a rendered frame yet.
            val received = receiver?.framesCompleted ?: 0L
            // A keyframe that was actually handed to the decoder is the only
            // evidence that separates a decoder fault from a network one.
            // framesCompleted cannot: on a lossy link the small P-frames
            // complete and inflate it while every keyframe - far bigger, spread
            // over many fragments, all of which VideoReceiver needs - is lost,
            // so a purely network problem would look exactly like a codec that
            // cannot decode and would be "fixed" by downgrading for nothing.
            val keyframeFed = decoder?.hasKeyframe == true
            val codecName = Protocol.codecName(codecChain[codecIndex])
            val canFallBack = codecIndex + 1 < codecChain.size

            // A keyframe went in and no picture came out. That is the decoder,
            // and the codec below may well handle it, so it gets a shorter fuse
            // than a diagnosis the user can do nothing about anyway.
            if (keyframeFed && canFallBack && inStage > UNDECODED_SECONDS) {
                stallReported = true
                fallBack(
                    "$codecName is not decoding on this device",
                    "A keyframe reached the decoder and no picture came out of it.",
                )
            } else if (inStage > DECODE_STALL_SECONDS) {
                stallReported = true
                // Outranks everything: whatever else went wrong, twenty seconds
                // without a picture is the thing worth telling the user about.
                // Three different faults land here and they need three different
                // answers - the note beside this headline already says how many
                // frames arrived, so a headline blaming the network while frames
                // are visibly arriving contradicts the screen it is printed on.
                val (headline, detail) = when {
                    received == 0L -> "No video is reaching this handheld" to
                        "The game is running on the PC, but no picture has arrived. " +
                        "The video port is probably blocked by a firewall or by this network."

                    !keyframeFed -> "No complete keyframe is getting through" to
                        "$received frames have arrived, but not one keyframe has arrived whole. " +
                        "A keyframe is many times the size of the frames around it, so this is " +
                        "the shape of a lossy or congested network rather than a codec fault."

                    else -> "This handheld is not decoding the stream" to
                        "$received frames have arrived from the PC and none of them decoded, " +
                        "including at least one keyframe. $codecName is the last codec left to try."
                }
                showLoadingError(headline, detail, ERROR_RANK_DIAGNOSED)
            }
        }
    }

    private fun elapsedText(millis: Long): String {
        val seconds = millis / 1000
        // Locale.US: this is a stopwatch, and locale digits would make it
        // unreadable next to the ASCII status text it sits beside.
        return String.format(Locale.US, "%d:%02d", seconds / 60, seconds % 60)
    }

    private fun hideLoading() {
        if (!loadingVisible) return
        loadingVisible = false
        handler.removeCallbacks(loadingTicker)
        // Full rather than the stage's 98% ceiling, so the last thing the bar
        // ever shows is a finished one.
        binding.loadingBar.progress = binding.loadingBar.max
        clearLoadingError()
        binding.loadingOverlay.visibility = View.GONE
    }

    /**
     * Failures stay on the full-screen overlay. Putting them in the corner
     * TextView leaves the user staring at the black screen this whole overlay
     * exists to replace. Once the stream is up the overlay is gone and errors
     * belong in the corner again, so this does nothing then.
     *
     * Deliberately not a latch: the ticker keeps running, so a decoder or a
     * connection that recovers still retires the overlay when a picture lands.
     *
     * Ranked rather than first-wins. Fallout must not shout down its own cause -
     * one failed gamepad write drops the control channel and would otherwise
     * mask the host error that follows - but the ranking has to cut the other
     * way too, or a transient codec exception at three seconds silences the
     * stall diagnosis at twenty, which is the one message that explains a
     * blocked video port.
     */
    private fun showLoadingError(headline: String, detail: String, rank: Int) {
        if (!loadingVisible) return
        if (loadingErrorShown && rank <= loadingErrorRank) return
        loadingErrorShown = true
        loadingErrorRank = rank
        binding.loadingStage.text = headline
        binding.loadingError.text = detail
        binding.loadingError.visibility = View.VISIBLE
    }

    private fun clearLoadingError() {
        if (!loadingErrorShown) return
        loadingErrorShown = false
        loadingErrorRank = ERROR_RANK_NONE
        binding.loadingError.visibility = View.GONE
    }

    // ---- keyboard ------------------------------------------------------------

    /**
     * The hidden field the soft keyboard types into. Characters are forwarded as
     * text; keys the host needs as keystrokes rather than characters - Enter,
     * Backspace, arrows - are forwarded as virtual key codes instead.
     */
    private fun setUpKeyboardSink() {
        binding.keyboardSink.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                if (count > before && s != null) {
                    val added = s.subSequence(start + before, start + count).toString()
                    if (added.isNotEmpty()) control?.sendText(added)
                }
            }
            override fun afterTextChanged(s: Editable?) {
                // Keep it empty so the next keystroke is always an insertion at 0
                // and we never re-send what has already gone.
                if (!s.isNullOrEmpty()) s.clear()
            }
        })

        binding.keyboardSink.setOnKeyListener { _, keyCode, event ->
            val virtualKey = windowsVirtualKey(keyCode) ?: return@setOnKeyListener false
            control?.sendKey(virtualKey, event.action == KeyEvent.ACTION_DOWN)
            true
        }
    }

    private fun toggleKeyboard() {
        val manager = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
        keyboardVisible = !keyboardVisible

        if (keyboardVisible) {
            binding.keyboardSink.requestFocus()
            manager.showSoftInput(binding.keyboardSink, InputMethodManager.SHOW_IMPLICIT)
            setStatus("Keyboard open · Select+Start to close")
        } else {
            manager.hideSoftInputFromWindow(binding.keyboardSink.windowToken, 0)
            binding.surface.requestFocus()
            goImmersive()
        }
    }

    /** Only the keys that must arrive as keystrokes rather than as text. */
    private fun windowsVirtualKey(keyCode: Int): Int? = when (keyCode) {
        KeyEvent.KEYCODE_ENTER -> 0x0D
        KeyEvent.KEYCODE_DEL -> 0x08
        KeyEvent.KEYCODE_ESCAPE -> 0x1B
        KeyEvent.KEYCODE_TAB -> 0x09
        KeyEvent.KEYCODE_DPAD_LEFT -> 0x25
        KeyEvent.KEYCODE_DPAD_UP -> 0x26
        KeyEvent.KEYCODE_DPAD_RIGHT -> 0x27
        KeyEvent.KEYCODE_DPAD_DOWN -> 0x28
        else -> null
    }

    // ---- input ---------------------------------------------------------------

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        // Armed exactly while the overlay is up, not just when it shows an
        // error: the overlay is opaque, so nothing the pad sends can be seen
        // anyway, and a stream that never produces a picture is otherwise a dead
        // end - the tracker turns B (and a controller's BACK) into a button
        // press for a game the user cannot see. The instant the overlay goes,
        // the pad gets B back exactly as before, which is what keeps B from
        // ending a live stream.
        if (loadingVisible && !keyboardVisible &&
            (event.keyCode == KeyEvent.KEYCODE_BUTTON_B || event.keyCode == KeyEvent.KEYCODE_BACK)
        ) {
            if (event.action == KeyEvent.ACTION_DOWN) finish()
            return true
        }

        // Select+Start opens the on-screen keyboard. Track both buttons and
        // swallow them while the combination is held, so the game never sees a
        // stray Start press from someone reaching for the keyboard.
        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_SELECT ||
            event.keyCode == KeyEvent.KEYCODE_BUTTON_START
        ) {
            val down = event.action == KeyEvent.ACTION_DOWN
            if (event.keyCode == KeyEvent.KEYCODE_BUTTON_SELECT) selectHeld = down
            if (event.keyCode == KeyEvent.KEYCODE_BUTTON_START) startHeld = down

            if (selectHeld && startHeld && down && !comboConsumed) {
                comboConsumed = true
                toggleKeyboard()
                return true
            }
            if (!selectHeld && !startHeld) comboConsumed = false
            // While the combo is armed, do not pass the individual button on.
            if (comboConsumed) return true
        }

        // The soft keyboard needs the events when it is open.
        if (keyboardVisible && event.keyCode != KeyEvent.KEYCODE_BUTTON_SELECT &&
            event.keyCode != KeyEvent.KEYCODE_BUTTON_START
        ) {
            return super.dispatchKeyEvent(event)
        }

        // The gamepad gets first refusal on everything, including BACK. That
        // ordering is the fix for B ending the stream: a B press that reaches
        // super unconsumed is turned into BACK by Android's key fallback, and
        // BACK finishes the activity. Consuming it here means it never happens.
        if (gamepad.onKey(event)) return true

        // A BACK the tracker declined did not come from the controller, so it is
        // the system gesture and should still leave the stream.
        if (event.keyCode == KeyEvent.KEYCODE_BACK) return super.dispatchKeyEvent(event)

        // Anything else unrecognised is a mapping gap. Say so on screen rather
        // than dropping it, because a button that does nothing is otherwise
        // indistinguishable from a broken connection.
        gamepad.lastUnmapped?.let {
            if (event.action == KeyEvent.ACTION_DOWN) setStatus("unmapped: $it")
        }
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (gamepad.onMotion(event)) return true
        return super.dispatchGenericMotionEvent(event)
    }

    // ---- lifecycle -----------------------------------------------------------

    private fun goImmersive() {
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = (
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                or View.SYSTEM_UI_FLAG_FULLSCREEN
                or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            )
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) goImmersive()
    }

    /**
     * Lets go of anything the finger was holding when we lose the foreground.
     *
     * ACTION_CANCEL covers the notification shade and Home, but it is not
     * promised for screen-off, a task swiped away, or a low-memory kill - and a
     * mouse button left down on the PC is not a fault anything downstream can
     * recover from, so it gets a second guarantee rather than one.
     */
    override fun onPause() {
        super.onPause()
        if (::touch.isInitialized) touch.cancel()
    }

    override fun onDestroy() {
        // Everything here happens before super, which cancels lifecycleScope -
        // and the read loop runs on it, so a teardown done afterwards races the
        // cancellation. Outbound messages are safe either way: they are drained
        // by ControlConnection's own writer thread, which belongs to no scope.
        sessionStarted = false
        pendingSession = null
        handler.removeCallbacksAndMessages(null)
        // Handed to stopSessionAndClose rather than released here: releasing
        // through the ordinary path queues behind a socket this method is about
        // to drop, so the release would be written to a null socket and lost -
        // leaving the left button held on the user's PC with nothing on either
        // side that would ever let it go.
        val heldButtons = if (::touch.isInitialized) touch.abandon() else emptyList()
        control?.stopSessionAndClose(heldButtons)
        receiver?.stop()
        decoder?.stop()
        super.onDestroy()
    }

    private fun nowMicros(): Long = System.nanoTime() / 1000

    companion object {
        private const val TAG = "StreamActivity"
        private const val IDR_THROTTLE_MS = 100L
        private const val LOADING_TICK_MS = 250L

        // Generous: the first keyframe normally lands in under a second, and a
        // keyframe request is retried every second until it does.
        private const val DECODE_STALL_SECONDS = 20f

        // Shorter, because video is demonstrably arriving and the only question
        // left is whether this device's decoder can do anything with it. Still
        // several keyframe requests' worth of patience.
        private const val UNDECODED_SECONDS = 8f

        // Long enough for the overlay to repaint with the reason before the
        // teardown blocks the main thread joining threads.
        private const val FALLBACK_DELAY_MS = 400L

        // What may replace what on the overlay, worst-explained to best.
        private const val ERROR_RANK_NONE = -1
        private const val ERROR_RANK_FALLOUT = 0
        private const val ERROR_RANK_REPORTED = 1
        private const val ERROR_RANK_DIAGNOSED = 2
        private const val NO_CONTROLLER = "no game controller detected"

        // The host fits the picture to an even size, so "the size we asked for"
        // is only ever exact to within a pixel on each axis.
        private const val SIZE_SLACK = 2

        const val EXTRA_HOST = "host"
        const val EXTRA_WINDOW_ID = "windowId"
        const val EXTRA_GAME_ID = "gameId"
        const val EXTRA_TITLE = "title"
        const val EXTRA_BITRATE = "bitrate"
        const val EXTRA_FPS = "fps"
    }
}
