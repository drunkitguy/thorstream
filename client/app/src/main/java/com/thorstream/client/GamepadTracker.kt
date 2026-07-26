package com.thorstream.client

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import kotlin.math.abs
import kotlin.math.roundToInt

data class GamepadState(
    var buttons: Int = 0,
    var leftTrigger: Int = 0,
    var rightTrigger: Int = 0,
    var leftStickX: Int = 0,
    var leftStickY: Int = 0,
    var rightStickX: Int = 0,
    var rightStickY: Int = 0,
    var sequence: Int = 0,
) {
    fun contentEquals(other: GamepadState): Boolean =
        buttons == other.buttons &&
            leftTrigger == other.leftTrigger &&
            rightTrigger == other.rightTrigger &&
            leftStickX == other.leftStickX &&
            leftStickY == other.leftStickY &&
            rightStickX == other.rightStickX &&
            rightStickY == other.rightStickY

    fun copyFrom(other: GamepadState) {
        buttons = other.buttons
        leftTrigger = other.leftTrigger
        rightTrigger = other.rightTrigger
        leftStickX = other.leftStickX
        leftStickY = other.leftStickY
        rightStickX = other.rightStickX
        rightStickY = other.rightStickY
    }
}

/**
 * Translates Android input events into the XInput-shaped state the host expects.
 *
 * Only emits when something actually changed, so a resting controller costs no
 * bandwidth and no host-side work.
 */
class GamepadTracker(private val onChanged: (GamepadState) -> Unit) {

    private val current = GamepadState()
    private val lastSent = GamepadState()
    private var sequence = 0

    /**
     * The last key this tracker did not recognise, for the on-screen readout.
     * Handhelds vary enormously in what key codes their built-in controls emit,
     * and a button that silently does nothing is impossible to diagnose from the
     * other end of a network.
     */
    var lastUnmapped: String? = null
        private set

    /** @return true if the event was a gamepad button we consumed. */
    fun onKey(event: KeyEvent): Boolean {
        val bit = buttonBitFor(event.keyCode, event.isFromController())
        if (bit == null) {
            if (event.action == KeyEvent.ACTION_DOWN) {
                lastUnmapped = "${KeyEvent.keyCodeToString(event.keyCode)} " +
                    "(code ${event.keyCode}, source 0x${Integer.toHexString(event.source)})"
            }
            return false
        }
        val pressed = event.action == KeyEvent.ACTION_DOWN

        // Some pads report triggers as buttons rather than axes.
        when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_L2 -> current.leftTrigger = if (pressed) 255 else 0
            KeyEvent.KEYCODE_BUTTON_R2 -> current.rightTrigger = if (pressed) 255 else 0
        }

        current.buttons = if (pressed) current.buttons or bit else current.buttons and bit.inv()
        emitIfChanged()
        return true
    }

    /** @return true if the event came from a joystick and we consumed it. */
    fun onMotion(event: MotionEvent): Boolean {
        if (!event.isFromSource(InputDevice.SOURCE_JOYSTICK) &&
            !event.isFromSource(InputDevice.SOURCE_GAMEPAD)
        ) {
            return false
        }
        if (event.action != MotionEvent.ACTION_MOVE) return false

        current.leftStickX = toAxis(event, MotionEvent.AXIS_X)
        // Android's Y axis points down; XInput's points up.
        current.leftStickY = -toAxis(event, MotionEvent.AXIS_Y)
        current.rightStickX = toAxis(event, MotionEvent.AXIS_Z)
        current.rightStickY = -toAxis(event, MotionEvent.AXIS_RZ)

        current.leftTrigger = toTrigger(event, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_BRAKE)
        current.rightTrigger = toTrigger(event, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS)

        // Many pads report the d-pad as a hat axis rather than as key events.
        val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        setBit(GamepadButton.DPAD_LEFT, hatX < -0.5f)
        setBit(GamepadButton.DPAD_RIGHT, hatX > 0.5f)
        setBit(GamepadButton.DPAD_UP, hatY < -0.5f)
        setBit(GamepadButton.DPAD_DOWN, hatY > 0.5f)

        emitIfChanged()
        return true
    }

    /** Called on a timer so the host can tell a quiet client from a dead one. */
    fun heartbeat() {
        sequence++
        current.sequence = sequence
        onChanged(current.copy())
    }

    private fun setBit(bit: Int, on: Boolean) {
        current.buttons = if (on) current.buttons or bit else current.buttons and bit.inv()
    }

    private fun emitIfChanged() {
        if (current.contentEquals(lastSent)) return
        lastSent.copyFrom(current)
        sequence++
        current.sequence = sequence
        onChanged(current.copy())
    }

    private fun toAxis(event: MotionEvent, axis: Int): Int {
        val raw = event.getAxisValue(axis)
        // A small deadzone keeps stick drift from streaming constant updates.
        val value = if (abs(raw) < DEADZONE) 0f else raw
        return (value.coerceIn(-1f, 1f) * 32767f).roundToInt().coerceIn(-32767, 32767)
    }

    private fun toTrigger(event: MotionEvent, primary: Int, fallback: Int): Int {
        var value = event.getAxisValue(primary)
        if (value == 0f) value = event.getAxisValue(fallback)
        return (value.coerceIn(0f, 1f) * 255f).roundToInt()
    }

    private fun buttonBitFor(keyCode: Int, fromController: Boolean): Int? = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER -> GamepadButton.A
        KeyEvent.KEYCODE_BUTTON_B -> GamepadButton.B
        // Two different routes end up here. Some handhelds' key layouts report
        // the B button as BACK outright; and Android's key-fallback turns an
        // unconsumed BUTTON_B into BACK by itself. Either way, a BACK that came
        // from a controller is the B button and must never leave the stream.
        // A BACK from anywhere else is the system gesture and is left alone.
        KeyEvent.KEYCODE_BACK -> if (fromController) GamepadButton.B else null
        KeyEvent.KEYCODE_BUTTON_X -> GamepadButton.X
        KeyEvent.KEYCODE_BUTTON_Y -> GamepadButton.Y
        KeyEvent.KEYCODE_BUTTON_L1 -> GamepadButton.LEFT_SHOULDER
        KeyEvent.KEYCODE_BUTTON_R1 -> GamepadButton.RIGHT_SHOULDER
        KeyEvent.KEYCODE_BUTTON_L2 -> null // handled as a trigger, not a button bit
        KeyEvent.KEYCODE_BUTTON_R2 -> null
        KeyEvent.KEYCODE_BUTTON_THUMBL -> GamepadButton.LEFT_THUMB
        KeyEvent.KEYCODE_BUTTON_THUMBR -> GamepadButton.RIGHT_THUMB
        KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU -> GamepadButton.START
        KeyEvent.KEYCODE_BUTTON_SELECT -> GamepadButton.BACK
        KeyEvent.KEYCODE_DPAD_UP -> GamepadButton.DPAD_UP
        KeyEvent.KEYCODE_DPAD_DOWN -> GamepadButton.DPAD_DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> GamepadButton.DPAD_LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> GamepadButton.DPAD_RIGHT
        else -> null
    }

    private companion object {
        const val DEADZONE = 0.12f
    }
}

/**
 * Whether this key came from a controller. Checked against the originating
 * device as well as the event source: some handhelds deliver their built-in
 * buttons with a KEYBOARD source even though the device itself is a gamepad.
 */
fun KeyEvent.isFromController(): Boolean =
    isFromSource(InputDevice.SOURCE_GAMEPAD) ||
        isFromSource(InputDevice.SOURCE_JOYSTICK) ||
        device?.isGameController() == true

/** True if this device looks like a game controller rather than a keyboard. */
fun InputDevice.isGameController(): Boolean {
    val sources = sources
    return (sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
        (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
}
