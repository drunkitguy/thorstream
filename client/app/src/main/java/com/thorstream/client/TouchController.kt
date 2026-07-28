package com.thorstream.client

import android.graphics.RectF
import android.os.Handler
import android.os.Looper
import android.view.Choreographer
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import kotlin.math.abs
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Turns touches on the video surface into mouse input on the host.
 *
 * Absolute, not trackpad-relative. The first attempt at this moved the pointer
 * by finger deltas, which cannot work here: the host captures with the cursor
 * switched off (options.captureCursor = false, host/src/session.cpp), so there
 * is nothing on screen to aim, and the client's idea of where the pointer sat
 * was a guess that any game recentring the cursor invalidated immediately. With
 * an absolute mapping the finger is the pointer, so nothing needs to be visible.
 *
 * - tap                    left click there
 * - drag                   move with the left button held
 * - long press             right click there
 * - two-finger vertical    scroll wheel
 */
class TouchController(
    private val enabled: () -> Boolean,
    private val onMove: (x: Int, y: Int) -> Unit,
    private val onButton: (button: Int, pressed: Boolean) -> Unit,
    private val onScroll: (delta: Int) -> Unit,
) {
    private var videoWidth = 0
    private var videoHeight = 0
    private val videoRect = RectF()

    private var gestureActive = false
    private var dragging = false
    private var scrolling = false
    private var longPressFired = false
    private var downX = 0f
    private var downY = 0f
    private var lastScrollY = 0f
    private var scrollAccumulator = 0f
    private var scrollStepPixels = 0f

    // Latest position not yet on the wire, and the last one that went, both in
    // the host's 0..65535 space. -1 means "nothing pending" / "nothing sent".
    private var pendingX = -1
    private var pendingY = -1
    private var sentX = -1
    private var sentY = -1
    private var flushScheduled = false
    private var lastSendNanos = 0L

    private val handler = Handler(Looper.getMainLooper())
    private val choreographer = Choreographer.getInstance()
    private val frameCallback = Choreographer.FrameCallback { frameTimeNanos -> flush(frameTimeNanos) }

    private val longPress = Runnable {
        if (!gestureActive || dragging || scrolling) return@Runnable
        longPressFired = true
        // Press and release together: Windows opens a context menu on the button
        // going up, so holding it down would only delay the menu until the
        // finger lifts, with nothing to show for the wait.
        press(BUTTON_RIGHT)
        release(BUTTON_RIGHT)
    }

    /** The encoded size the host reported in STARTED; sets the picture's shape. */
    fun setVideoSize(width: Int, height: Int) {
        videoWidth = width
        videoHeight = height
    }

    fun onTouch(view: View, event: MotionEvent): Boolean {
        // The loading overlay is opaque and covers the whole screen, but it is
        // not clickable, so touches fall straight through to this surface. A
        // touch on something the user cannot see must not reach the PC.
        if (!enabled()) {
            cancel()
            return false
        }

        val rect = videoRectIn(view) ?: return false
        if (scrollStepPixels <= 0f) {
            scrollStepPixels = view.resources.displayMetrics.density * SCROLL_STEP_DP
        }
        val slop = ViewConfiguration.get(view.context).scaledTouchSlop.toFloat()

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                // The letterbox bars are not the PC. Clamping a touch there to
                // the nearest edge would put the pointer somewhere the user
                // never pointed, so the whole gesture is declined instead.
                if (!rect.contains(event.x, event.y)) return false
                // Should be unreachable - every path out of a gesture releases -
                // but a button held into a new gesture is the one failure with
                // no way back, so it costs nothing to insure against it here.
                if (dragging) release(BUTTON_LEFT)
                gestureActive = true
                dragging = false
                scrolling = false
                longPressFired = false
                downX = event.x
                downY = event.y
                // Forget what was last sent: a game is free to have moved the
                // cursor since, so a second tap on the same pixel still has to
                // say where it is rather than being deduplicated away.
                sentX = -1
                sentY = -1
                movePointer(event.x, event.y, rect)
                handler.postDelayed(longPress, LONG_PRESS_MS)
            }

            MotionEvent.ACTION_POINTER_DOWN -> {
                if (!gestureActive) return false
                handler.removeCallbacks(longPress)
                // A second finger makes this a scroll. Any held button has to be
                // let go first or it stays down for the rest of the gesture.
                if (dragging) {
                    release(BUTTON_LEFT)
                    dragging = false
                }
                scrolling = true
                scrollAccumulator = 0f
                lastScrollY = averageY(event, -1)
            }

            MotionEvent.ACTION_MOVE -> {
                if (!gestureActive) return false
                if (scrolling) {
                    scrollBy(averageY(event, -1))
                    return true
                }

                // A finger that has wandered into a bar holds the pointer where
                // it was rather than dragging it to an edge it never reached.
                if (!rect.contains(event.x, event.y)) return true

                if (!dragging && !longPressFired &&
                    (abs(event.x - downX) > slop || abs(event.y - downY) > slop)
                ) {
                    // Past the slop this is a drag, and a drag is the button
                    // held down: that is what drag-select and drag-to-look are.
                    handler.removeCallbacks(longPress)
                    dragging = true
                    press(BUTTON_LEFT)
                }
                movePointer(event.x, event.y, rect)
            }

            MotionEvent.ACTION_POINTER_UP -> {
                if (!gestureActive) return false
                // Re-baseline without the finger that is leaving, or the average
                // jumps and the stream scrolls by itself.
                if (scrolling) lastScrollY = averageY(event, event.actionIndex)
            }

            MotionEvent.ACTION_UP -> {
                if (!gestureActive) return false
                handler.removeCallbacks(longPress)
                when {
                    dragging -> release(BUTTON_LEFT)
                    // A scroll or a long press has already had its effect.
                    scrolling || longPressFired -> Unit
                    else -> {
                        movePointer(event.x, event.y, rect)
                        press(BUTTON_LEFT)
                        release(BUTTON_LEFT)
                    }
                }
                reset()
            }

            MotionEvent.ACTION_CANCEL -> cancel()

            else -> return gestureActive
        }
        return true
    }

    /** Releases anything held, for a gesture that will get no more events. */
    fun cancel() {
        for (button in abandon()) release(button)
    }

    /**
     * Drops the gesture without sending anything, reporting what it was holding.
     *
     * For teardown, where the ordinary send path is about to be taken away: the
     * caller passes the result to ControlConnection.stopSessionAndClose, which
     * gets it out on the thread that survives.
     */
    fun abandon(): List<Int> {
        handler.removeCallbacks(longPress)
        choreographer.removeFrameCallback(frameCallback)
        flushScheduled = false
        pendingX = -1
        pendingY = -1
        val held = if (dragging) listOf(BUTTON_LEFT) else emptyList()
        reset()
        return held
    }

    private fun reset() {
        gestureActive = false
        dragging = false
        scrolling = false
        longPressFired = false
    }

    /**
     * Where the picture actually is inside [view].
     *
     * The same fit StreamActivity gives the SurfaceView, recomputed from live
     * geometry so a rotation or a resize cannot leave the two disagreeing.
     */
    private fun videoRectIn(view: View): RectF? {
        val viewWidth = view.width.toFloat()
        val viewHeight = view.height.toFloat()
        if (viewWidth <= 0f || viewHeight <= 0f) return null
        if (videoWidth <= 0 || videoHeight <= 0) return null

        val scale = min(viewWidth / videoWidth, viewHeight / videoHeight)
        val width = videoWidth * scale
        val height = videoHeight * scale
        val left = (viewWidth - width) / 2f
        val top = (viewHeight - height) / 2f
        videoRect.set(left, top, left + width, top + height)
        return videoRect
    }

    /**
     * Queues a position, normalised 0..65535 across the host's virtual desktop.
     *
     * That is the space MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK works in,
     * and the host arranges for the captured picture to be exactly the whole of
     * it: the game's client area is placed over the whole virtual display and
     * the physical displays are detached (host/src/session.cpp). So the video
     * rect maps onto the full range, and only the letterbox is off the map.
     */
    private fun movePointer(x: Float, y: Float, rect: RectF) {
        pendingX = ((x - rect.left) / rect.width() * RANGE).roundToInt().coerceIn(0, RANGE.toInt())
        pendingY = ((y - rect.top) / rect.height() * RANGE).roundToInt().coerceIn(0, RANGE.toInt())
        if (flushScheduled) return
        flushScheduled = true
        choreographer.postFrameCallback(frameCallback)
    }

    /**
     * One move per frame at most, and never the position already sent.
     *
     * A finger produces motion events faster than the panel refreshes, and every
     * message is a separate write on a control channel that does not serialise
     * its writers. Coalescing here is what keeps that from being a flood.
     */
    private fun flush(frameTimeNanos: Long) {
        flushScheduled = false
        if (pendingX < 0) return
        if (frameTimeNanos - lastSendNanos < MIN_SEND_INTERVAL_NANOS) {
            flushScheduled = true
            choreographer.postFrameCallback(frameCallback)
            return
        }
        sendPending(frameTimeNanos)
    }

    /** Sends any queued position now, so a click lands where the finger is. */
    private fun sendPending(nowNanos: Long) {
        val x = pendingX
        val y = pendingY
        pendingX = -1
        pendingY = -1
        if (x < 0 || (x == sentX && y == sentY)) return
        sentX = x
        sentY = y
        lastSendNanos = nowNanos
        onMove(x, y)
    }

    private fun press(button: Int) {
        sendPending(System.nanoTime())
        onButton(button, true)
    }

    private fun release(button: Int) {
        sendPending(System.nanoTime())
        onButton(button, false)
    }

    private fun scrollBy(y: Float) {
        scrollAccumulator += y - lastScrollY
        lastScrollY = y
        while (abs(scrollAccumulator) >= scrollStepPixels) {
            val direction = if (scrollAccumulator > 0) 1 else -1
            // A forward notch is positive and shows earlier content, which is
            // what dragging two fingers down the screen should do.
            onScroll(direction * WHEEL_DELTA)
            scrollAccumulator -= direction * scrollStepPixels
        }
    }

    private fun averageY(event: MotionEvent, excludeIndex: Int): Float {
        var total = 0f
        var counted = 0
        for (index in 0 until event.pointerCount) {
            if (index == excludeIndex) continue
            total += event.getY(index)
            counted++
        }
        return if (counted == 0) lastScrollY else total / counted
    }

    private companion object {
        const val BUTTON_LEFT = 0
        const val BUTTON_RIGHT = 1

        const val RANGE = 65535f
        const val WHEEL_DELTA = 120

        // The platform's own value, so a long press feels the same here as it
        // does everywhere else on the handheld.
        val LONG_PRESS_MS = ViewConfiguration.getLongPressTimeout().toLong()

        // In dp rather than pixels: a notch per finger-width of travel should
        // not depend on how dense the panel is.
        const val SCROLL_STEP_DP = 24f

        // Choreographer is the real rate limit; this is only a backstop against
        // a panel faster than any of these. It has to sit clear BELOW the frame
        // interval of every rate we expect, or a frame gets skipped and the
        // panel sends half as often: 7ms would do that to 144 Hz (6.944ms) and
        // 6ms leaves 165 Hz (6.061ms) only 60 microseconds of margin, which
        // frame-time jitter would eat. 5ms clears 165 Hz comfortably. Rates are
        // always a whole fraction of the refresh, since sends only happen on a
        // frame boundary, so a 240 Hz panel (4.167ms) skips every other frame
        // and sends 120 a second rather than the 200 the interval alone implies.
        const val MIN_SEND_INTERVAL_NANOS = 5_000_000L
    }
}
