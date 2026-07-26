package com.thorstream.client

import android.view.MotionEvent
import android.view.View
import kotlin.math.abs

/**
 * Turns touches on the video surface into mouse input on the host.
 *
 * Deliberately trackpad-like rather than absolute: a finger is much bigger than
 * a cursor, so tapping where you want to click is imprecise and the finger hides
 * the target. Dragging moves the pointer relative to where it already was, which
 * is how touchpads work and is far easier to aim with.
 *
 * - drag one finger      move the pointer
 * - tap                  left click
 * - two-finger tap       right click
 * - two-finger drag      scroll
 */
class TouchController(
    private val onMove: (x: Int, y: Int) -> Unit,
    private val onButton: (button: Int, pressed: Boolean) -> Unit,
    private val onScroll: (delta: Int) -> Unit,
) {
    // Normalised 0..65535, the coordinate space the host expects. Start centred.
    private var pointerX = 32767f
    private var pointerY = 32767f

    private var lastX = 0f
    private var lastY = 0f
    private var downTime = 0L
    private var movedFar = false
    private var pointerCount = 0
    private var scrollAccumulator = 0f

    fun onTouch(view: View, event: MotionEvent): Boolean {
        val width = view.width.toFloat().coerceAtLeast(1f)
        val height = view.height.toFloat().coerceAtLeast(1f)

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                lastX = event.x
                lastY = event.y
                downTime = System.currentTimeMillis()
                movedFar = false
                pointerCount = 1
                scrollAccumulator = 0f
            }

            MotionEvent.ACTION_POINTER_DOWN -> {
                pointerCount = event.pointerCount
                movedFar = false
            }

            MotionEvent.ACTION_MOVE -> {
                val deltaX = event.x - lastX
                val deltaY = event.y - lastY
                lastX = event.x
                lastY = event.y

                if (abs(deltaX) > MOVE_THRESHOLD || abs(deltaY) > MOVE_THRESHOLD) movedFar = true

                if (event.pointerCount >= 2) {
                    // Two fingers: scroll. One notch per WHEEL_STEP pixels, and
                    // accumulate so slow drags still eventually scroll.
                    scrollAccumulator += deltaY
                    while (abs(scrollAccumulator) >= WHEEL_STEP) {
                        val direction = if (scrollAccumulator > 0) 1 else -1
                        onScroll(direction * 120)
                        scrollAccumulator -= direction * WHEEL_STEP
                    }
                } else {
                    // Scale movement to the surface so a swipe crosses a sensible
                    // fraction of the screen regardless of panel size.
                    pointerX = (pointerX + deltaX / width * 65535f * SENSITIVITY)
                        .coerceIn(0f, 65535f)
                    pointerY = (pointerY + deltaY / height * 65535f * SENSITIVITY)
                        .coerceIn(0f, 65535f)
                    onMove(pointerX.toInt(), pointerY.toInt())
                }
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val quick = System.currentTimeMillis() - downTime < TAP_MILLIS
                if (event.actionMasked == MotionEvent.ACTION_UP && quick && !movedFar) {
                    // A tap that never moved is a click, at wherever the pointer
                    // currently is - not where the finger landed.
                    val button = if (pointerCount >= 2) 1 else 0
                    onButton(button, true)
                    onButton(button, false)
                }
                if (event.actionMasked == MotionEvent.ACTION_UP) pointerCount = 0
            }
        }
        return true
    }

    private companion object {
        const val SENSITIVITY = 1.6f
        const val MOVE_THRESHOLD = 12f
        const val TAP_MILLIS = 250L
        const val WHEEL_STEP = 40f
    }
}
