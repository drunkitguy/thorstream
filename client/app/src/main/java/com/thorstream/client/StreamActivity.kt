package com.thorstream.client

import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.DisplayMetrics
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.View
import android.view.WindowManager
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.thorstream.client.databinding.ActivityStreamBinding
import kotlinx.coroutines.launch

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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityStreamBinding.inflate(layoutInflater)
        setContentView(binding.root)

        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        goImmersive()

        binding.title.text = intent.getStringExtra(EXTRA_TITLE) ?: "streaming"

        gamepad = GamepadTracker { state -> control?.sendGamepad(state) }

        binding.surface.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                surfaceReady = true
                connect()
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

        connection.onStarted = { session ->
            runOnUiThread {
                if (!surfaceReady) return@runOnUiThread
                setStatus("Decoding ${session.width}x${session.height}")
                val videoDecoder = VideoDecoder(binding.surface.holder.surface)
                videoDecoder.onError = { message ->
                    runOnUiThread { setStatus("Decoder error: $message") }
                }
                try {
                    videoDecoder.start(session)
                    decoder = videoDecoder
                    sessionStarted = true
                    // Anything the host sent before this moment went nowhere,
                    // including the keyframe the stream opened with. Ask for a
                    // fresh one rather than waiting for a decoder that will never
                    // produce a picture.
                    control?.requestKeyframe()
                    lastIdrRequest = System.currentTimeMillis()
                    startStatsTicker()
                } catch (e: Exception) {
                    Log.e(TAG, "decoder failed to start", e)
                    setStatus("Could not start the decoder: ${e.message}")
                }
            }
        }
        connection.onLaunchProgress = { message -> runOnUiThread { setStatus(message) } }
        connection.onError = { message -> runOnUiThread { setStatus("Host: $message") } }
        connection.onDisconnected = { runOnUiThread { setStatus("Disconnected from host.") } }
        connection.onPong = { sentMicros ->
            latencyMillis = (nowMicros() - sentMicros) / 1000
        }

        setStatus("Connecting...")
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
                }
            } catch (e: Exception) {
                runOnUiThread { setStatus("Could not connect: ${e.message}") }
            }
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
                decoder?.let { if (!it.hasKeyframe) requestKeyframeThrottled() }
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
        if (activeDecoder != null && !activeDecoder.hasKeyframe) {
            setStatus(
                if (frames == 0L) "no video received — check the network"
                else "waiting for a keyframe · $frames frames received"
            )
            return
        }

        val latency = if (latencyMillis >= 0) "${latencyMillis}ms rtt" else "…"
        setStatus("%.0f fps · %s · %d dropped".format(fps, latency, videoReceiver.framesDropped))
    }

    private fun setStatus(message: String) {
        binding.status.text = message
    }

    // ---- input ---------------------------------------------------------------

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        // Let the back button out so the user can leave the stream.
        if (event.keyCode == KeyEvent.KEYCODE_BACK) return super.dispatchKeyEvent(event)
        if (gamepad.onKey(event)) return true
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
        super.onDestroy()
        sessionStarted = false
        handler.removeCallbacksAndMessages(null)
        control?.stopSession()
        control?.close()
        receiver?.stop()
        decoder?.stop()
    }

    private fun nowMicros(): Long = System.nanoTime() / 1000

    companion object {
        private const val TAG = "StreamActivity"
        private const val IDR_THROTTLE_MS = 100L

        const val EXTRA_HOST = "host"
        const val EXTRA_WINDOW_ID = "windowId"
        const val EXTRA_GAME_ID = "gameId"
        const val EXTRA_TITLE = "title"
        const val EXTRA_BITRATE = "bitrate"
        const val EXTRA_FPS = "fps"
    }
}
