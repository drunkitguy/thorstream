#include "input_injector.h"

#include <atomic>
#include <vector>

namespace thorstream {
namespace {

// Returns whether the whole batch actually went in. SendInput is blocked
// wholesale by UIPI whenever a more privileged window owns the input desktop -
// a UAC prompt, the secure desktop, some anti-cheat overlays - and a press that
// never landed must not be recorded as held.
bool Send(INPUT* inputs, UINT count) {
    return SendInput(count, inputs, sizeof(INPUT)) == count;
}

// Bitmask of the mouse buttons the client currently has down. Written from the
// network thread as events arrive and read from whichever thread notices the
// client has gone, so it is atomic - the operations are single bit flips and
// need no more than that.
std::atomic<uint32_t> g_heldButtons{0};

constexpr uint32_t ButtonBit(InputInjector::MouseButton button) {
    return 1u << static_cast<uint32_t>(button);
}

}  // namespace

void InputInjector::MoveMouse(uint16_t normalisedX, uint16_t normalisedY) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = normalisedX;
    input.mi.dy = normalisedY;
    // VIRTUALDESK matters once the physical displays are detached: the virtual
    // display is then the whole desktop, and coordinates must be relative to
    // that rather than to whichever monitor Windows considers primary.
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    Send(&input, 1);
}

void InputInjector::MouseButtonEvent(MouseButton button, bool pressed) {
    INPUT input{};
    input.type = INPUT_MOUSE;

    switch (button) {
        case MouseButton::Left:
            input.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case MouseButton::Right:
            input.mi.dwFlags = pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case MouseButton::Middle:
            input.mi.dwFlags = pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        default:
            return;
    }

    // Bit set before the down, cleared after the up. The release side is what
    // this ordering makes safe: ReleaseHeldButtons landing mid-release either
    // takes a bit whose up is already on its way, or finds it cleared - never a
    // button left down with no bit.
    //
    // The press side is not equally safe, and pretending otherwise would be
    // worse than the gap. A ReleaseHeldButtons that lands between the fetch_or
    // and the Send below takes the bit, injects an up for a button that is not
    // down yet, and then the down runs - leaving it held with no bit to release
    // it by. Closing that needs the mark and the injection to be one atomic
    // step, which SendInput cannot be part of. The window is two instructions
    // wide and requires a disconnect precisely inside it; the alternative
    // ordering loses real presses on every disconnect, so this is the better
    // trade rather than a guarantee.
    if (pressed) g_heldButtons.fetch_or(ButtonBit(button), std::memory_order_relaxed);
    const bool injected = Send(&input, 1);

    // The bit means "physically down", which is intent AND delivery, so it is
    // cleared exactly when those two disagree:
    //   press  delivered  -> down, bit stays set
    //   press  swallowed  -> never went down, clear it
    //   up     delivered  -> no longer down, clear it
    //   up     swallowed  -> STILL DOWN, keep the bit
    // That last row is why this is not `!pressed || !injected`: UIPI taking the
    // input desktop mid-drag swallows the button-up, and clearing there would
    // forget a button that is genuinely held - the exact leak this tracking
    // exists to prevent, reached from the other side.
    if (pressed != injected) {
        g_heldButtons.fetch_and(~ButtonBit(button), std::memory_order_relaxed);
    }
}

void InputInjector::ReleaseHeldButtons() {
    // Exchange rather than load-then-clear: whoever takes the bits owns them, so
    // two callers racing on the same disconnect cannot both synthesise an up for
    // the same button.
    const uint32_t held = g_heldButtons.exchange(0, std::memory_order_relaxed);
    if (held == 0) return;

    // One call per button rather than one batched call. SendInput reports how
    // many events it injected but not which, so a partial failure in a batch
    // cannot be attributed to a button - and the bits are already gone from the
    // exchange above, so anything unattributable would be dropped for good.
    const auto lift = [](MouseButton button, DWORD flag) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flag;
        if (Send(&input, 1)) return;
        // Blocked, so it is still down. Put the bit back rather than losing it:
        // the client's own button-up, or the next disconnect, can then still
        // lift it once the privileged window goes away.
        g_heldButtons.fetch_or(ButtonBit(button), std::memory_order_relaxed);
    };

    if (held & ButtonBit(MouseButton::Left)) lift(MouseButton::Left, MOUSEEVENTF_LEFTUP);
    if (held & ButtonBit(MouseButton::Right)) lift(MouseButton::Right, MOUSEEVENTF_RIGHTUP);
    if (held & ButtonBit(MouseButton::Middle)) lift(MouseButton::Middle, MOUSEEVENTF_MIDDLEUP);
}

void InputInjector::Scroll(int16_t delta) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta);
    Send(&input, 1);
}

void InputInjector::KeyEvent(uint16_t virtualKey, bool pressed) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtualKey;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = pressed ? 0 : KEYEVENTF_KEYUP;

    // Extended keys need the flag or they arrive as their numpad equivalents.
    switch (virtualKey) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR:  case VK_NEXT:   case VK_LEFT: case VK_RIGHT:
        case VK_UP:     case VK_DOWN:   case VK_NUMLOCK: case VK_RCONTROL:
        case VK_RMENU:
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            break;
        default:
            break;
    }

    Send(&input, 1);
}

void InputInjector::TypeText(const std::wstring& text) {
    if (text.empty()) return;

    std::vector<INPUT> inputs;
    inputs.reserve(text.size() * 2);

    for (const wchar_t character : text) {
        // Surrogate pairs are sent as two units; Windows reassembles them.
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = static_cast<WORD>(character);
        down.ki.dwFlags = KEYEVENTF_UNICODE;

        INPUT up = down;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

        inputs.push_back(down);
        inputs.push_back(up);
    }

    Send(inputs.data(), static_cast<UINT>(inputs.size()));
}

}  // namespace thorstream
