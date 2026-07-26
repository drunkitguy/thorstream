#include "input_injector.h"

#include <vector>

namespace thorstream {
namespace {

void Send(INPUT* inputs, UINT count) {
    SendInput(count, inputs, sizeof(INPUT));
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
    Send(&input, 1);
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
