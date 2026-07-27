package com.thorstream.client

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
import kotlin.math.exp

/** The actual streaming screen: video in, gamepad out. */
class StreamActivity : AppCompatActivity() {

    private lateinit var binding: ActivityStreamBinding

    private var control: ControlConnection? = null
    private var receiver: VideoReceiver? = null
    private var decoder: VideoDecoder? = null
    private lateinit var gamepad: GamepadTracker

    private val handler = Handler(Looper.getMainLooper())
    private var surfaceReady = false
    private var sessionStarted = false
    private var lastIdrRequest = 0L
    private var latencyMillis = -1L

    private var lastStatsTime = 0L
    private var lastStatsFrames = 0L
    private var lastStatsBytes = 0L

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

        setUpKeyboardSink()

        binding.surface.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                surfaceReady = true
                connect()
                // STARTED can land while the surface is gone - Home during a
                // three-minute launch - and connect() will not retry because the
                // control connection is still up. Without this the session is
                // stranded with no decoder and nothing to show for it.
                pendingSession?.let { session -> startDecoder(session) }
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
        val bitrate = intent.getIntExtra(EXTRA_BITRATE, 30000)
        val fps = intent.getIntExtra(EXTRA_FPS, 60)

        val videoReceiver = VideoReceiver { frame -> decoder?.submit(frame) }
        val udpPort = videoReceiver.bind()
        videoReceiver.onFrameLost = { requestKeyframeThrottled() }
        videoReceiver.start()
        receiver = videoReceiver

        val connection = ControlConnection(lifecycleScope)
        control = connection

        connection.onStarted = { session -> runOnUiThread { startDecoder(session) } }
        connection.onLaunchProgress = { message ->
            runOnUiThread {
                setStatus(message)
                showLaunchProgress(message)
            }
        }
        connection.onError = { message ->
            runOnUiThread {
                setStatus("Host: $message")
                showLoadingError("The PC could not start the game", message, ERROR_RANK_REPORTED)
            }
        }
        connection.onDisconnected = {
            runOnUiThread {
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
            latencyMillis = (nowMicros() - sentMicros) / 1000
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
                        codec = Protocol.CODEC_H264,
                        udpPort = udpPort,
                    )
                } else {
                    connection.requestStart(
                        windowId = windowId,
                        width = maxWidth,
                        height = maxHeight,
                        fps = fps,
                        bitrateKbps = bitrate,
                        codec = Protocol.CODEC_H264,
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
    private fun startDecoder(session: SessionInfo) {
        // STARTED can land after teardown: runOnUiThread still delivers once the
        // activity is destroyed. Configuring a codec against a dead surface
        // leaks it, and the stats ticker it would start reposts itself onto an
        // already-flushed handler, holding the activity for good.
        if (isFinishing || isDestroyed) return
        if (decoder != null) return
        if (!surfaceReady) {
            pendingSession = session
            loadingNote = "Waiting for the display to come back"
            return
        }
        pendingSession = null

        setStatus("Decoding ${session.width}x${session.height}")
        val videoDecoder = VideoDecoder(binding.surface.holder.surface)
        videoDecoder.onError = { message ->
            runOnUiThread {
                setStatus("Decoder error: $message")
                showLoadingError("The video decoder failed", message, ERROR_RANK_REPORTED)
            }
        }
        try {
            videoDecoder.start(session)
            decoder = videoDecoder
            sessionStarted = true
            // STARTED is not a picture. This is the stage the complaint was
            // about: the host is streaming but nothing has decoded yet, so the
            // overlay stays until something has.
            enterStage(
                LoadStage.DECODING,
                "Waiting for the first frame",
                "Decoding ${session.width}x${session.height}",
            )
            // Anything the host sent before this moment went nowhere, including
            // the keyframe the stream opened with. Ask for a fresh one rather
            // than waiting for a decoder that will never produce a picture.
            control?.requestKeyframe()
            lastIdrRequest = System.currentTimeMillis()
            startStatsTicker()
        } catch (e: Exception) {
            Log.e(TAG, "decoder failed to start", e)
            setStatus("Could not start the decoder: ${e.message}")
            showLoadingError(
                "Could not start the video decoder",
                e.message ?: "unknown error",
                ERROR_RANK_REPORTED,
            )
        }
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

    private fun startStatsTicker() {
        lastStatsTime = System.currentTimeMillis()
        handler.post(object : Runnable {
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
        })
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
        setStatus("%.0f fps · %s · %d dropped".format(fps, latency, videoReceiver.framesDropped))
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
        if (loadStage == LoadStage.DECODING && !stallReported && inStage > DECODE_STALL_SECONDS) {
            stallReported = true
            // Outranks everything: whatever else went wrong, twenty seconds
            // without a picture is the thing worth telling the user about.
            showLoadingError(
                "No video is reaching this handheld",
                "The game is running on the PC, but no picture has arrived. " +
                    "The video port is probably blocked by a firewall or by this network.",
                ERROR_RANK_DIAGNOSED,
            )
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

    override fun onDestroy() {
        // Everything here happens before super, which cancels lifecycleScope -
        // and every ordinary send is dispatched on that scope, so a teardown
        // done afterwards is a teardown the host never hears about.
        sessionStarted = false
        pendingSession = null
        handler.removeCallbacksAndMessages(null)
        control?.stopSessionAndClose()
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

        // What may replace what on the overlay, worst-explained to best.
        private const val ERROR_RANK_NONE = -1
        private const val ERROR_RANK_FALLOUT = 0
        private const val ERROR_RANK_REPORTED = 1
        private const val ERROR_RANK_DIAGNOSED = 2
        private const val NO_CONTROLLER = "no game controller detected"

        const val EXTRA_HOST = "host"
        const val EXTRA_WINDOW_ID = "windowId"
        const val EXTRA_GAME_ID = "gameId"
        const val EXTRA_TITLE = "title"
        const val EXTRA_BITRATE = "bitrate"
        const val EXTRA_FPS = "fps"
    }
}
