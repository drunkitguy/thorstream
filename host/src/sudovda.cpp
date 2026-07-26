#include "sudovda.h"

#include <setupapi.h>

#include <vector>

#pragma comment(lib, "setupapi.lib")

namespace thorstream::sudovda {
namespace {

// The driver's IOCTLs are synchronous from our point of view, but the device is
// opened overlapped, so every call needs an event to wait on.
bool DeviceIoctl(HANDLE device, DWORD code, void* input, DWORD inputSize, void* output,
                 DWORD outputSize) {
    if (device == INVALID_HANDLE_VALUE) return false;

    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) return false;

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(device, code, input, inputSize, output, outputSize, &returned,
                              &overlapped);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(device, &overlapped, &returned, TRUE);
    }

    CloseHandle(overlapped.hEvent);
    return ok != FALSE;
}

}  // namespace

HANDLE OpenDriver() {
    const HDEVINFO devices =
        SetupDiGetClassDevsW(&kInterfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    HANDLE handle = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA interfaceData{};
    interfaceData.cbSize = sizeof(interfaceData);

    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(devices, nullptr, &kInterfaceGuid, index, &interfaceData);
         ++index) {
        DWORD detailSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &detailSize, nullptr);
        if (detailSize == 0) continue;

        std::vector<uint8_t> buffer(detailSize);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, detailSize,
                                              &detailSize, nullptr)) {
            continue;
        }

        handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (handle != INVALID_HANDLE_VALUE) break;
    }

    SetupDiDestroyDeviceInfoList(devices);
    return handle;
}

bool QueryProtocolVersion(HANDLE device, ProtocolVersion* out) {
    return DeviceIoctl(device, kIoctlGetProtocolVersion, nullptr, 0, out, sizeof(*out));
}

bool QueryWatchdog(HANDLE device, WatchdogOut* out) {
    return DeviceIoctl(device, kIoctlGetWatchdog, nullptr, 0, out, sizeof(*out));
}

bool Ping(HANDLE device) {
    return DeviceIoctl(device, kIoctlPing, nullptr, 0, nullptr, 0);
}

bool AddDisplay(HANDLE device, const AddDisplayParams& params, AddDisplayOut* out) {
    auto mutableParams = params;
    return DeviceIoctl(device, kIoctlAddDisplay, &mutableParams, sizeof(mutableParams), out,
                       sizeof(*out));
}

bool RemoveDisplay(HANDLE device, const GUID& monitorGuid) {
    RemoveDisplayParams params{monitorGuid};
    return DeviceIoctl(device, kIoctlRemoveDisplay, &params, sizeof(params), nullptr, 0);
}

}  // namespace thorstream::sudovda
