#include "gamepad_vigem.h"

#include <Windows.h>

#include "ViGEm/Client.h"

namespace thorstream {
namespace {
// A client that reconnects restarts its sequence at 1. Only treat a lower
// sequence as stale if it is a small step backwards, not a restart.
constexpr uint32_t kSequenceWrapGuard = 1024;
}  // namespace

struct VirtualGamepad::Impl {
    PVIGEM_CLIENT client = nullptr;
    PVIGEM_TARGET pad = nullptr;

    ~Impl() {
        if (client && pad) {
            vigem_target_remove(client, pad);
        }
        if (pad) vigem_target_free(pad);
        if (client) {
            vigem_disconnect(client);
            vigem_free(client);
        }
    }
};

VirtualGamepad::VirtualGamepad() : impl_(std::make_unique<Impl>()) {}
VirtualGamepad::~VirtualGamepad() = default;

std::unique_ptr<VirtualGamepad> VirtualGamepad::Create(std::string* error) {
    auto self = std::unique_ptr<VirtualGamepad>(new VirtualGamepad());
    auto& impl = *self->impl_;

    impl.client = vigem_alloc();
    if (!impl.client) {
        if (error) *error = "out of memory allocating the ViGEm client";
        return nullptr;
    }

    const VIGEM_ERROR connectResult = vigem_connect(impl.client);
    if (!VIGEM_SUCCESS(connectResult)) {
        if (error) {
            *error = (connectResult == VIGEM_ERROR_BUS_NOT_FOUND)
                         ? "the ViGEmBus driver is not installed - gamepad input will not work"
                         : "vigem_connect failed (0x" + std::to_string(connectResult) + ")";
        }
        return nullptr;
    }

    impl.pad = vigem_target_x360_alloc();
    if (!impl.pad) {
        if (error) *error = "failed to allocate a virtual Xbox 360 pad";
        return nullptr;
    }

    const VIGEM_ERROR addResult = vigem_target_add(impl.client, impl.pad);
    if (!VIGEM_SUCCESS(addResult)) {
        if (error) *error = "vigem_target_add failed (0x" + std::to_string(addResult) + ")";
        return nullptr;
    }

    return self;
}

void VirtualGamepad::Submit(const protocol::GamepadState& state) {
    auto& impl = *impl_;
    if (!impl.client || !impl.pad) return;

    // Input arrives over TCP so it is already ordered, but a reconnecting client
    // restarts its counter; only reject what is plainly stale.
    if (state.sequence != 0 && state.sequence < lastSequence_ &&
        lastSequence_ - state.sequence < kSequenceWrapGuard) {
        return;
    }
    lastSequence_ = state.sequence;

    // The protocol deliberately uses the XInput button layout, so the button
    // bitfield maps across with no translation table to get wrong.
    XUSB_REPORT report{};
    report.wButtons = state.buttons;
    report.bLeftTrigger = state.leftTrigger;
    report.bRightTrigger = state.rightTrigger;
    report.sThumbLX = state.leftStickX;
    report.sThumbLY = state.leftStickY;
    report.sThumbRX = state.rightStickX;
    report.sThumbRY = state.rightStickY;

    vigem_target_x360_update(impl.client, impl.pad, report);
}

}  // namespace thorstream
