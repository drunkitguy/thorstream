#pragma once

#include <Windows.h>
// WIN32_LEAN_AND_MEAN drops this, and CTL_CODE lives here.
#include <winioctl.h>

#include <cstdint>

// SudoVDA (SudoMaker Virtual Display Adapter) control interface.
//
// Reimplemented from the driver's published IOCTL codes and structure layouts
// rather than vendored from Apollo, whose wrapper is GPL-3.0. What is reproduced
// here is the driver ABI - the numbers and byte layouts needed to talk to it -
// not any of their implementation.
//
// The driver is installed by Apollo/Sunshine. If it is absent, everything here
// fails cleanly and the host carries on without a virtual display.
namespace thorstream::sudovda {

// {e5bcc234-1e0c-418a-a0d4-ef8b7501414d}
inline constexpr GUID kInterfaceGuid = {
    0xe5bcc234, 0x1e0c, 0x418a, {0xa0, 0xd4, 0xef, 0x8b, 0x75, 0x01, 0x41, 0x4d}};

inline constexpr DWORD kIoctlAddDisplay = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS);
inline constexpr DWORD kIoctlRemoveDisplay = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS);
inline constexpr DWORD kIoctlSetRenderAdapter = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS);
inline constexpr DWORD kIoctlGetWatchdog = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS);
inline constexpr DWORD kIoctlPing = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x888, METHOD_BUFFERED, FILE_ANY_ACCESS);
inline constexpr DWORD kIoctlGetProtocolVersion = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8FF, METHOD_BUFFERED, FILE_ANY_ACCESS);

#pragma pack(push, 1)
struct ProtocolVersion {
    uint8_t major;
    uint8_t minor;
    uint8_t incremental;
    bool testBuild;
};

struct AddDisplayParams {
    UINT width;
    UINT height;
    UINT refreshRate;
    GUID monitorGuid;
    CHAR deviceName[14];
    CHAR serialNumber[14];
};

struct AddDisplayOut {
    LUID adapterLuid;
    UINT targetId;
};

struct RemoveDisplayParams {
    GUID monitorGuid;
};

struct WatchdogOut {
    UINT timeout;
    UINT countdown;
};
#pragma pack(pop)

// INVALID_HANDLE_VALUE if the driver is not installed or not reachable.
HANDLE OpenDriver();

bool QueryProtocolVersion(HANDLE device, ProtocolVersion* out);
bool QueryWatchdog(HANDLE device, WatchdogOut* out);
bool Ping(HANDLE device);

bool AddDisplay(HANDLE device, const AddDisplayParams& params, AddDisplayOut* out);
bool RemoveDisplay(HANDLE device, const GUID& monitorGuid);

}  // namespace thorstream::sudovda
