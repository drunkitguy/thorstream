#include "display_topology.h"

#include <shlobj.h>

#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace thorstream {
namespace {

std::filesystem::path MarkerPath() {
    PWSTR appData = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData) != S_OK) return {};
    std::filesystem::path path = appData;
    CoTaskMemFree(appData);
    return path / L"thorstream" / L"detached-displays.bin";
}

}  // namespace

std::vector<SavedDisplay> DisplayTopology::Snapshot() {
    std::vector<SavedDisplay> result;

    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
        if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;

        SavedDisplay saved;
        saved.deviceName = device.DeviceName;
        saved.primary = (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        saved.mode.dmSize = sizeof(DEVMODEW);
        if (EnumDisplaySettingsExW(device.DeviceName, ENUM_CURRENT_SETTINGS, &saved.mode, 0)) {
            result.push_back(std::move(saved));
        }
    }
    return result;
}

bool DisplayTopology::Attach(const std::wstring& deviceName, int width, int height,
                             int refreshRate, std::string* error) {
    // Park it to the right of everything currently attached, so attaching it does
    // not disturb the existing arrangement before we detach anyway.
    LONG rightEdge = 0;
    for (const auto& display : Snapshot()) {
        rightEdge = (std::max)(rightEdge,
                               display.mode.dmPosition.x +
                                   static_cast<LONG>(display.mode.dmPelsWidth));
    }

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    mode.dmPelsWidth = static_cast<DWORD>(width);
    mode.dmPelsHeight = static_cast<DWORD>(height);
    mode.dmBitsPerPel = 32;
    mode.dmDisplayFrequency = static_cast<DWORD>(refreshRate);
    mode.dmPosition.x = rightEdge;
    mode.dmPosition.y = 0;
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY | DM_POSITION;

    LONG result = ChangeDisplaySettingsExW(deviceName.c_str(), &mode, nullptr,
                                           CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        // Some drivers reject an explicit refresh rate; the mode matters more.
        mode.dmFields &= ~DM_DISPLAYFREQUENCY;
        result = ChangeDisplaySettingsExW(deviceName.c_str(), &mode, nullptr,
                                          CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    }
    if (result != DISP_CHANGE_SUCCESSFUL) {
        if (error) *error = "could not attach the virtual display (code " + std::to_string(result) + ")";
        return false;
    }

    const LONG applied = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (applied != DISP_CHANGE_SUCCESSFUL) {
        if (error) *error = "attaching the virtual display was rejected (code " +
                            std::to_string(applied) + ")";
        return false;
    }
    return true;
}

bool DisplayTopology::KeepOnly(const std::wstring& keepDeviceName, std::string* error) {
    if (keepDeviceName.empty()) {
        if (error) *error = "no display to keep";
        return false;
    }

    const auto displays = Snapshot();
    bool keepFound = false;
    for (const auto& display : displays) {
        if (display.deviceName == keepDeviceName) keepFound = true;
    }
    if (!keepFound) {
        // Refuse rather than detach everything and leave nothing behind.
        if (error) *error = "the display to keep is not attached; refusing to detach the others";
        return false;
    }

    // ChangeDisplaySettingsEx cannot express "only this display": it refuses to
    // detach whichever display is primary, and reports success while doing
    // nothing. The CCD API describes the whole topology in one call instead,
    // which is what the Settings app uses, so use that.
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
        if (error) *error = "could not read the display configuration";
        return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount, modes.data(),
                           nullptr) != ERROR_SUCCESS) {
        if (error) *error = "could not enumerate display paths";
        return false;
    }
    paths.resize(pathCount);
    modes.resize(modeCount);

    // Submit only the path we want active. Handing back the whole QDC_ALL_PATHS
    // set - which contains many unusable inactive paths - is rejected outright
    // with ERROR_INVALID_PARAMETER.
    std::vector<DISPLAYCONFIG_PATH_INFO> keeperOnly;
    bool keeperFound = false;
    for (auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;

        const bool isKeeper = DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
                              keepDeviceName == source.viewGdiDeviceName;

        if (!isKeeper) continue;

        path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
        // Mode indices refer to the array we are no longer supplying, so clear
        // them and let Windows compute a mode for the one remaining path.
        path.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        keeperOnly.push_back(path);
        keeperFound = true;
        break;
    }

    if (!keeperFound) {
        if (error) *error = "the display to keep has no display path; refusing to detach the rest";
        return false;
    }

    // SDC_ALLOW_CHANGES lets Windows pick consistent modes and positions for the
    // single remaining path, which then necessarily becomes primary at the origin.
    const LONG applied =
        SetDisplayConfig(static_cast<UINT32>(keeperOnly.size()), keeperOnly.data(), 0, nullptr,
                         SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES |
                             SDC_SAVE_TO_DATABASE);
    if (applied != ERROR_SUCCESS) {
        if (error) *error = "SetDisplayConfig rejected the change (code " + std::to_string(applied) + ")";
        return false;
    }

    // Verify rather than trust the return code; this API is more honest than
    // ChangeDisplaySettingsEx, but the whole point here is not to be wrong.
    for (const auto& display : Snapshot()) {
        if (display.deviceName != keepDeviceName) {
            if (error) *error = "Windows kept " + std::string(display.deviceName.begin(),
                                                             display.deviceName.end()) +
                                " attached despite reporting success";
            return false;
        }
    }
    return true;
}

bool DisplayTopology::Restore(const std::vector<SavedDisplay>& snapshot) {
    if (snapshot.empty()) return RestoreDefaultTopology();

    // Primary first, so the others have a coordinate space to be placed around.
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& display : snapshot) {
            if (display.primary != (pass == 0)) continue;

            DEVMODEW mode = display.mode;
            mode.dmSize = sizeof(mode);
            mode.dmFields |= DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;

            DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET;
            if (display.primary) flags |= CDS_SET_PRIMARY;
            ChangeDisplaySettingsExW(display.deviceName.c_str(), &mode, nullptr, flags, nullptr);
        }
    }

    const LONG applied = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (applied == DISP_CHANGE_SUCCESSFUL) return true;

    // An exact restore failed; getting *a* usable desktop back matters more than
    // getting the original arrangement back.
    return RestoreDefaultTopology();
}

bool DisplayTopology::RestoreDefaultTopology() {
    return SetDisplayConfig(0, nullptr, 0, nullptr,
                            SDC_APPLY | SDC_TOPOLOGY_EXTEND) == ERROR_SUCCESS ||
           SetDisplayConfig(0, nullptr, 0, nullptr,
                            SDC_APPLY | SDC_TOPOLOGY_INTERNAL) == ERROR_SUCCESS;
}

void DisplayTopology::MarkDetached(const std::vector<SavedDisplay>& snapshot) {
    const auto path = MarkerPath();
    if (path.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return;

    const uint32_t count = static_cast<uint32_t>(snapshot.size());
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& display : snapshot) {
        const uint32_t nameLength = static_cast<uint32_t>(display.deviceName.size());
        file.write(reinterpret_cast<const char*>(&nameLength), sizeof(nameLength));
        file.write(reinterpret_cast<const char*>(display.deviceName.data()),
                   nameLength * sizeof(wchar_t));
        file.write(reinterpret_cast<const char*>(&display.mode), sizeof(display.mode));
        file.write(reinterpret_cast<const char*>(&display.primary), sizeof(display.primary));
    }
}

void DisplayTopology::ClearMarker() {
    std::error_code ec;
    std::filesystem::remove(MarkerPath(), ec);
}

bool DisplayTopology::RecoverIfMarked() {
    const auto path = MarkerPath();
    if (path.empty()) return false;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;

    wprintf(L"Displays were left detached by a previous run; restoring them.\n");

    std::vector<SavedDisplay> snapshot;
    {
        std::ifstream file(path, std::ios::binary);
        uint32_t count = 0;
        if (file.read(reinterpret_cast<char*>(&count), sizeof(count)) && count < 64) {
            for (uint32_t i = 0; i < count; ++i) {
                SavedDisplay display;
                uint32_t nameLength = 0;
                if (!file.read(reinterpret_cast<char*>(&nameLength), sizeof(nameLength))) break;
                if (nameLength > 256) break;
                display.deviceName.resize(nameLength);
                file.read(reinterpret_cast<char*>(display.deviceName.data()),
                          nameLength * sizeof(wchar_t));
                file.read(reinterpret_cast<char*>(&display.mode), sizeof(display.mode));
                file.read(reinterpret_cast<char*>(&display.primary), sizeof(display.primary));
                snapshot.push_back(std::move(display));
            }
        }
    }

    const bool restored = Restore(snapshot);
    ClearMarker();
    return restored;
}

}  // namespace thorstream
