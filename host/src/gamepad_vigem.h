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

    // Returns false if the driver rejected the update; `error` explains why.
    bool Submit(const protocol::GamepadState& state, std::string* error = nullptr);

    // Zeroes every button and axis. Called when a client goes away so a
    // disconnect mid-press does not leave the game holding forward forever.
    void ReleaseAll();

    // The XInput slot the driver assigned, or -1 if it has not reported one.
    // Worth logging: if this is -1, games will never see the pad no matter how
    // happily the driver accepts updates.
    int XInputSlot() const;

private:
    struct Impl;
    VirtualGamepad();

    std::unique_ptr<Impl> impl_;
};

}  // namespace thorstream
