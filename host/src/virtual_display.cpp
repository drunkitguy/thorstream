#include "virtual_display.h"

#include <objbase.h>

#include <chrono>
#include <cstdio>

#include "sudovda.h"

namespace thorstream {
namespace {

// The driver's watchdog is 3 seconds. Ping well inside that so a scheduling
// hiccup or a busy encoder thread cannot cost us the display mid-session.
constexpr auto kPingInterval = std::chrono::milliseconds(1000);

// Windows takes a moment to enumerate a freshly added monitor.
constexpr auto kEnumerateTimeout = std::chrono::seconds(5);

// Resolves the GDI device name of the path matching this adapter and target.
std::wstring FindDeviceName(const LUID& adapterLuid, UINT targetId) {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
        return {};
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount, modes.data(),
                           nullptr) != ERROR_SUCCESS) {
        return {};
    }

    for (UINT32 i = 0; i < pathCount; ++i) {
        const auto& path = paths[i];
        if (path.targetInfo.adapterId.LowPart != adapterLuid.LowPart ||
            path.targetInfo.adapterId.HighPart != adapterLuid.HighPart ||
            path.targetInfo.id != targetId) {
            continue;
        }

        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS) {
            return source.viewGdiDeviceName;
        }
    }
    return {};
}

}  // namespace

struct VirtualDisplay::Impl {
    HANDLE device = INVALID_HANDLE_VALUE;
    GUID monitorGuid{};
    bool added = false;

    ~Impl() {
        if (device != INVALID_HANDLE_VALUE) {
            if (added) sudovda::RemoveDisplay(device, monitorGuid);
            CloseHandle(device);
        }
    }
};

VirtualDisplay::VirtualDisplay() : impl_(std::make_unique<Impl>()) {}

VirtualDisplay::~VirtualDisplay() {
    running_ = false;
    if (pingThread_.joinable()) pingThread_.join();
    // Impl's destructor removes the display and closes the handle.
}

bool VirtualDisplay::IsDriverAvailable() {
    const HANDLE device = sudovda::OpenDriver();
    if (device == INVALID_HANDLE_VALUE) return false;
    CloseHandle(device);
    return true;
}

std::unique_ptr<VirtualDisplay> VirtualDisplay::Create(const VirtualDisplayRequest& request,
                                                       std::string* error) {
    const auto fail = [&](std::string message) -> std::unique_ptr<VirtualDisplay> {
        if (error) *error = std::move(message);
        return nullptr;
    };

    auto self = std::unique_ptr<VirtualDisplay>(new VirtualDisplay());
    self->request_ = request;
    auto& impl = *self->impl_;

    impl.device = sudovda::OpenDriver();
    if (impl.device == INVALID_HANDLE_VALUE) {
        return fail("the SudoVDA virtual display driver is not installed");
    }

    sudovda::ProtocolVersion version{};
    if (!sudovda::QueryProtocolVersion(impl.device, &version)) {
        return fail("the virtual display driver did not report a protocol version");
    }

    if (CoCreateGuid(&impl.monitorGuid) != S_OK) return fail("could not allocate a monitor id");

    sudovda::AddDisplayParams params{};
    params.width = static_cast<UINT>(request.width);
    params.height = static_cast<UINT>(request.height);
    params.refreshRate = static_cast<UINT>(request.refreshRate);
    params.monitorGuid = impl.monitorGuid;
    // Both fields are fixed-size and not NUL-terminated by the driver's contract.
    strncpy_s(params.deviceName, sizeof(params.deviceName), "thorstream", _TRUNCATE);
    strncpy_s(params.serialNumber, sizeof(params.serialNumber), "THOR0001", _TRUNCATE);

    sudovda::AddDisplayOut out{};
    if (!sudovda::AddDisplay(impl.device, params, &out)) {
        return fail("the driver refused to add a virtual display (error " +
                    std::to_string(GetLastError()) + ")");
    }
    impl.added = true;

    // Start pinging immediately: the watchdog is already counting down, and it
    // will remove the display we just made if we take too long to find it.
    self->running_ = true;
    self->pingThread_ = std::thread(&VirtualDisplay::PingLoop, self.get());

    const auto deadline = std::chrono::steady_clock::now() + kEnumerateTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        self->deviceName_ = FindDeviceName(out.adapterLuid, out.targetId);
        if (!self->deviceName_.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (self->deviceName_.empty()) {
        return fail("the virtual display was created but Windows never enumerated it");
    }

    return self;
}

void VirtualDisplay::PingLoop() {
    while (running_) {
        if (!sudovda::Ping(impl_->device)) {
            wprintf(L"virtual display ping failed; the driver may drop the display\n");
        }
        // Sleep in slices so shutdown does not wait a full interval.
        for (int i = 0; i < 10 && running_; ++i) {
            std::this_thread::sleep_for(kPingInterval / 10);
        }
    }
}

}  // namespace thorstream
