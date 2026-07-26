#include "gamepad_vigem.h"

#include <Windows.h>

#include "ViGEm/Client.h"

namespace thorstream {
namespace {
// A client that reconnects restarts its sequence at 1. Only treat a lower
// sequence as stale if it is a small step backwards, not a restart.
constexpr int kAddAttempts = 5;
constexpr DWORD kAddRetryDelayMs = 400;

// VIGEM_ERROR is a negative-valued enum, so std::to_string produces things like
// "0x-536870905". Format the actual 32-bit pattern, and name the codes a user
// can act on.
std::string ErrorText(VIGEM_ERROR error) {
    switch (error) {
        case VIGEM_ERROR_BUS_NOT_FOUND:
            return "the ViGEmBus driver is not installed - run: winget install ViGEm.ViGEmBus";
        case VIGEM_ERROR_NO_FREE_SLOT:
            return "no free controller slot - too many virtual pads are already attached";
        case VIGEM_ERROR_BUS_VERSION_MISMATCH:
            return "the installed ViGEmBus driver is too old for this client";
        case VIGEM_ERROR_ALREADY_CONNECTED:
            return "that virtual pad is already attached";
        case VIGEM_ERROR_TARGET_NOT_PLUGGED_IN:
            return "the virtual pad was not plugged in - a previous host may not have shut down "
                   "cleanly; unplugging it can take the driver a few seconds";
        default: {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(error));
            return std::string("ViGEm error ") + buffer;
        }
    }
}
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
        if (error) *error = ErrorText(connectResult);
        return nullptr;
    }

    impl.pad = vigem_target_x360_alloc();
    if (!impl.pad) {
        if (error) *error = "failed to allocate a virtual Xbox 360 pad";
        return nullptr;
    }

    // If a previous host was killed rather than shut down, the driver can hold
    // its slot for a moment. Retry briefly instead of giving up on input for the
    // whole session.
    VIGEM_ERROR addResult = VIGEM_ERROR_NONE;
    for (int attempt = 0; attempt < kAddAttempts; ++attempt) {
        addResult = vigem_target_add(impl.client, impl.pad);
        if (VIGEM_SUCCESS(addResult)) return self;
        Sleep(kAddRetryDelayMs);
    }

    if (error) *error = ErrorText(addResult);
    return nullptr;
}

bool VirtualGamepad::Submit(const protocol::GamepadState& state, std::string* error) {
    auto& impl = *impl_;
    if (!impl.client || !impl.pad) {
        if (error) *error = "virtual pad is not attached";
        return false;
    }

    // No sequence filtering: input arrives over TCP, which is already ordered,
    // and there is only ever one client. Rejecting "older" sequence numbers
    // silently broke every reconnecting client, because a fresh client starts
    // counting from 1 while the host still remembered the last one's counter.
    // The sequence field stays in the protocol for a future UDP input path.

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

    const VIGEM_ERROR result = vigem_target_x360_update(impl.client, impl.pad, report);
    if (!VIGEM_SUCCESS(result)) {
        if (error) *error = ErrorText(result);
        return false;
    }
    return true;
}

int VirtualGamepad::XInputSlot() const {
    auto& impl = *impl_;
    if (!impl.client || !impl.pad) return -1;

    ULONG index = 0;
    if (!VIGEM_SUCCESS(vigem_target_x360_get_user_index(impl.client, impl.pad, &index))) return -1;
    return static_cast<int>(index);
}

void VirtualGamepad::ReleaseAll() {
    auto& impl = *impl_;
    if (!impl.client || !impl.pad) return;
    vigem_target_x360_update(impl.client, impl.pad, XUSB_REPORT{});
}

}  // namespace thorstream
