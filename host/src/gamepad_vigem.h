#pragma once

#include <memory>
#include <string>

#include "protocol.h"

namespace thorstream {

// Presents a virtual Xbox 360 controller to Windows and feeds it the state the
// Thor sends. Games see an ordinary XInput pad - no per-game configuration, and
// no injecting keystrokes into whatever window happens to be focused.
class VirtualGamepad {
public:
    ~VirtualGamepad();

    VirtualGamepad(const VirtualGamepad&) = delete;
    VirtualGamepad& operator=(const VirtualGamepad&) = delete;

    // Returns nullptr with `error` set if the ViGEmBus driver is missing.
    static std::unique_ptr<VirtualGamepad> Create(std::string* error);

    void Submit(const protocol::GamepadState& state);

private:
    struct Impl;
    VirtualGamepad();

    std::unique_ptr<Impl> impl_;
    uint32_t lastSequence_ = 0;
};

}  // namespace thorstream
