#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

namespace thorstream {

// Mouse and keyboard input from the client, injected as though it came from real
// hardware. Separate from the gamepad, which goes through a virtual pad instead:
// games read controllers via XInput, but mice and keyboards through the normal
// input stack, so SendInput is the right route for these.
class InputInjector {
public:
    // Positions are normalised 0..65535 across the virtual desktop, so the client
    // never needs to know the encode resolution or where the window sits - and
    // they stay correct when the stream is scaled.
    static void MoveMouse(uint16_t normalisedX, uint16_t normalisedY);

    enum class MouseButton : uint8_t { Left = 0, Right = 1, Middle = 2 };
    static void MouseButtonEvent(MouseButton button, bool pressed);

    // Lifts whatever MouseButtonEvent still has held down. Nothing else can: by
    // the time a client is gone the socket that would have carried the matching
    // button-up is gone with it, and a button left down outlives the session, the
    // disconnect and the host process - it stays held until the user physically
    // clicks. Call this whenever a client goes away for any reason.
    //
    // Which buttons are held is tracked rather than assumed: synthesising three
    // button-ups unconditionally would deliver phantom clicks to whatever the
    // user has under the pointer, which is a worse bug than the one being fixed.
    static void ReleaseHeldButtons();

    // Positive scrolls up, in WHEEL_DELTA units.
    static void Scroll(int16_t delta);

    static void KeyEvent(uint16_t virtualKey, bool pressed);

    // Types text as Unicode, which avoids having to map characters to keyboard
    // layouts that may not even contain them.
    static void TypeText(const std::wstring& text);
};

}  // namespace thorstream
