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

// Device names are ASCII (\\.\DISPLAY9), so this is lossless for them.
std::string Narrow(const std::wstring& text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t character : text) {
        result.push_back(character < 128 ? static_cast<char>(character) : '?');
    }
    return result;
}

const wchar_t* ChangeResultName(LONG code) {
    switch (code) {
        case DISP_CHANGE_SUCCESSFUL:  return L"DISP_CHANGE_SUCCESSFUL";
        case DISP_CHANGE_RESTART:     return L"DISP_CHANGE_RESTART";
        case DISP_CHANGE_FAILED:      return L"DISP_CHANGE_FAILED";
        case DISP_CHANGE_BADMODE:     return L"DISP_CHANGE_BADMODE";
        case DISP_CHANGE_NOTUPDATED:  return L"DISP_CHANGE_NOTUPDATED";
        case DISP_CHANGE_BADFLAGS:    return L"DISP_CHANGE_BADFLAGS";
        case DISP_CHANGE_BADPARAM:    return L"DISP_CHANGE_BADPARAM";
        case DISP_CHANGE_BADDUALVIEW: return L"DISP_CHANGE_BADDUALVIEW";
        default:                      return L"DISP_CHANGE_?";
    }
}

// Everything about a DEVMODE that can make ChangeDisplaySettingsEx refuse it,
// in one line. dmFields is included raw because which fields were *set* is
// exactly the thing a driver rejects, and it cannot be inferred from the values.
std::wstring DescribeMode(const DEVMODEW& mode) {
    wchar_t buffer[256];
    swprintf_s(buffer, L"%ux%u @%uHz %ubpp at (%ld,%ld) fields=0x%08X",
               mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency, mode.dmBitsPerPel,
               mode.dmPosition.x, mode.dmPosition.y, static_cast<unsigned>(mode.dmFields));
    return buffer;
}

std::wstring DescribeFlags(DWORD flags) {
    std::wstring text;
    const auto add = [&](DWORD bit, const wchar_t* name) {
        if (!(flags & bit)) return;
        if (!text.empty()) text += L"|";
        text += name;
    };
    add(CDS_UPDATEREGISTRY, L"UPDATEREGISTRY");
    add(CDS_NORESET, L"NORESET");
    add(CDS_SET_PRIMARY, L"SET_PRIMARY");
    add(CDS_TEST, L"TEST");
    return text.empty() ? L"(none)" : text;
}

// How close the desktop is to `snapshot`.
//
// The grading exists to decide what is worth doing about a mismatch, and the
// two levels are not interchangeable: RestoreDefaultTopology can only choose
// which displays are ACTIVE, so it can plausibly bring a missing display back
// and can do nothing whatsoever about one that is merely in the wrong place.
//
// Displays that are *not* in the snapshot are allowed. At the end of a session
// the virtual display is usually still attached and about to be removed, and a
// check that failed on that would tear down a topology that is in fact correct.
DisplayTopology::RestoreOutcome CompareToSnapshot(const std::vector<SavedDisplay>& snapshot) {
    const auto current = DisplayTopology::Snapshot();
    bool missing = false;
    bool drifted = false;

    for (const auto& wanted : snapshot) {
        const SavedDisplay* found = nullptr;
        for (const auto& display : current) {
            if (display.deviceName == wanted.deviceName) found = &display;
        }
        if (!found) {
            wprintf(L"  %s did not come back\n", wanted.deviceName.c_str());
            missing = true;
            continue;
        }
        if (found->mode.dmPosition.x != wanted.mode.dmPosition.x ||
            found->mode.dmPosition.y != wanted.mode.dmPosition.y ||
            found->mode.dmPelsWidth != wanted.mode.dmPelsWidth ||
            found->mode.dmPelsHeight != wanted.mode.dmPelsHeight ||
            found->primary != wanted.primary) {
            wprintf(L"  %s came back as %ux%u at (%ld,%ld)%hs, wanted %ux%u at (%ld,%ld)%hs\n",
                    wanted.deviceName.c_str(), found->mode.dmPelsWidth, found->mode.dmPelsHeight,
                    found->mode.dmPosition.x, found->mode.dmPosition.y,
                    found->primary ? " [primary]" : "", wanted.mode.dmPelsWidth,
                    wanted.mode.dmPelsHeight, wanted.mode.dmPosition.x, wanted.mode.dmPosition.y,
                    wanted.primary ? " [primary]" : "");
            drifted = true;
            continue;
        }
        // Counted as drift rather than waved through. A 120Hz laptop panel that
        // comes back at 60Hz is glaringly obvious to the person looking at it,
        // and the remedy for it is the same cheap one as for a bad position:
        // drop the virtual display and restore again. What it must NOT do is
        // reach RestoreDefaultTopology, which cannot set a refresh rate either.
        if (found->mode.dmDisplayFrequency != wanted.mode.dmDisplayFrequency) {
            wprintf(L"  %s came back at %uHz rather than %uHz\n", wanted.deviceName.c_str(),
                    found->mode.dmDisplayFrequency, wanted.mode.dmDisplayFrequency);
            drifted = true;
        }
    }

    if (missing) return DisplayTopology::RestoreOutcome::Missing;
    if (drifted) return DisplayTopology::RestoreOutcome::Drifted;
    return DisplayTopology::RestoreOutcome::Restored;
}

// The GDI device name (\\.\DISPLAYn) behind a CCD path's source.
std::wstring SourceDeviceName(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) return {};
    return source.viewGdiDeviceName;
}

// Translates the whole arrangement so `deviceName` sits at the origin, using the
// API the Settings app uses. ChangeDisplaySettingsEx describes one display at a
// time and leaves the driver to agree that the intermediate states make sense;
// this describes the finished topology in a single call, so there are no
// intermediate states to refuse.
bool MakePrimaryViaDisplayConfig(const std::wstring& deviceName, std::string* error) {
    UINT32 pathCount = 0, modeCount = 0;
    LONG rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (rc != ERROR_SUCCESS) {
        *error = "GetDisplayConfigBufferSizes failed (" + std::to_string(rc) + ")";
        return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                            modes.data(), nullptr);
    if (rc != ERROR_SUCCESS) {
        *error = "QueryDisplayConfig failed (" + std::to_string(rc) + ")";
        return false;
    }
    paths.resize(pathCount);
    modes.resize(modeCount);

    POINTL origin{};
    bool found = false;
    for (const auto& path : paths) {
        if (SourceDeviceName(path) != deviceName) continue;
        const UINT32 index = path.sourceInfo.modeInfoIdx;
        if (index >= modes.size() || modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            continue;
        }
        origin = modes[index].sourceMode.position;
        found = true;
        break;
    }
    if (!found) {
        *error = "no active display path for " + Narrow(deviceName);
        return false;
    }

    for (auto& mode : modes) {
        if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
        mode.sourceMode.position.x -= origin.x;
        mode.sourceMode.position.y -= origin.y;
    }

    wprintf(L"  SetDisplayConfig: translating %u paths by (%ld,%ld)\n", pathCount, -origin.x,
            -origin.y);
    rc = SetDisplayConfig(pathCount, paths.data(), modeCount, modes.data(),
                          SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE);
    if (rc != ERROR_SUCCESS) {
        *error = "SetDisplayConfig rejected the translated topology (" + std::to_string(rc) + ")";
        return false;
    }
    return true;
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
        wprintf(L"Staging %s at %s was refused: %s (%ld)\n", deviceName.c_str(),
                DescribeMode(mode).c_str(), ChangeResultName(result), result);
        if (error) *error = "could not attach the virtual display (" +
                            Narrow(ChangeResultName(result)) + ", code " + std::to_string(result) +
                            ")";
        return false;
    }

    const LONG applied = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (applied != DISP_CHANGE_SUCCESSFUL) {
        if (error) *error = "attaching the virtual display was rejected (" +
                            Narrow(ChangeResultName(applied)) + ", code " +
                            std::to_string(applied) + ")";
        return false;
    }
    return true;
}

bool DisplayTopology::MakePrimary(const std::wstring& deviceName, std::string* error) {
    const auto displays = Snapshot();

    const SavedDisplay* target = nullptr;
    for (const auto& display : displays) {
        if (display.deviceName == deviceName) target = &display;
    }
    if (!target) {
        if (error) *error = "the display to make primary is not attached";
        return false;
    }

    // Windows defines the primary display as the one whose top-left corner is
    // the origin, so this is a translation of the whole arrangement rather than
    // a property of one display. CDS_SET_PRIMARY on a display that is not being
    // moved to (0,0) is quietly ignored.
    const LONG shiftX = -target->mode.dmPosition.x;
    const LONG shiftY = -target->mode.dmPosition.y;

    // Nothing to move. Worth checking rather than translating by (0,0), because
    // every route below is a real mode set that blanks the screens for a moment.
    if (target->primary && shiftX == 0 && shiftY == 0) {
        wprintf(L"%s is already the primary display at the origin\n", deviceName.c_str());
        return true;
    }

    // The arrangement being translated, printed before anything is attempted:
    // when this fails on a machine nobody can attach a debugger to, the starting
    // topology is the first thing needed to understand why.
    wprintf(L"Making %s primary; translating the desktop by (%ld,%ld)\n", deviceName.c_str(),
            shiftX, shiftY);
    for (const auto& display : displays) {
        wprintf(L"  %s %s%hs\n", display.deviceName.c_str(), DescribeMode(display.mode).c_str(),
                display.primary ? "  [primary]" : "");
    }

    const auto verify = [&]() -> bool {
        for (const auto& display : Snapshot()) {
            if (display.deviceName == deviceName && display.primary) return true;
        }
        return false;
    };

    // SetDisplayConfig first, because ChangeDisplaySettingsEx is at best
    // unreliable here. What the runs on the reporting machine actually
    // established, kept separate from what they suggest:
    //
    //   - the reported failure reproduces. The old walk - snapshot order, full
    //     mode - was refused with DISP_CHANGE_FAILED every time it was tried.
    //   - SetDisplayConfig succeeded on every attempt, with two displays and
    //     with three, including a panel at a negative y offset.
    //   - "new primary first, position only" succeeded once, on an otherwise
    //     untouched desktop, and was refused in every later run. But every one
    //     of those later runs followed a staging probe that had already had a
    //     call refused, and a refused call poisons the pending arrangement for
    //     the calls after it. So the probe is a plausible cause of its own
    //     results. Whether that plan is genuinely unreliable on this hardware
    //     or was merely poisoned is NOT established, and the probe cannot
    //     settle it: measuring the second combination requires recovering from
    //     the first, and recovering is itself a topology change.
    //
    // Which display gets named in a refusal follows the order they are staged
    // in - the probe was refused by DISPLAY5 when DISPLAY5 was staged first.
    // That is why the field log named the virtual display: not because anything
    // about it was wrong, but because it was the one holding the parcel when
    // the music stopped.
    //
    // The mechanism this points at: ChangeDisplaySettingsEx describes one
    // display at a time, so a rearrangement is a sequence of states in which the
    // desktop is half-moved and the origin belongs to nobody, and every driver
    // involved has a veto over each of them. The CCD API takes the finished
    // topology in a single call - it is what the Settings app uses to drag a
    // monitor around - so there is no intermediate state for anyone to object
    // to. It either applies atomically or changes nothing, so a failure here
    // costs nothing and leaves the fallbacks below a clean state to work from.
    wprintf(L"  trying: SetDisplayConfig\n");
    std::string ccdError;
    if (MakePrimaryViaDisplayConfig(deviceName, &ccdError)) {
        if (verify()) {
            wprintf(L"  %s is primary at the origin (SetDisplayConfig)\n", deviceName.c_str());
            return true;
        }
        ccdError = "SetDisplayConfig succeeded without making " + Narrow(deviceName) + " primary";
    }
    wprintf(L"    %hs\n", ccdError.c_str());

    struct Plan {
        const wchar_t* description;
        bool targetFirst;
        bool positionOnly;
    };

    // Fallbacks, for a driver or a Windows build that will not take a supplied
    // CCD topology. Only the last of these has been shown to fail here, and the
    // first has been seen to work once, so they are ordered by which is least
    // likely to be refused rather than by measurement. Treat them as untested
    // code for hardware nobody has: that is exactly why the recovery below has
    // to be careful.
    //
    // targetFirst: the old order's first act was to move the display that
    // currently *is* primary off the origin, leaving a pending arrangement with
    // nothing at (0,0) for every later call to be measured against. Designating
    // the new primary first gives them a fixed origin.
    //
    // positionOnly: nothing here changes resolution, refresh rate or depth, and
    // a DEVMODE carrying DM_PELSWIDTH/DM_DISPLAYFREQUENCY/DM_BITSPERPEL is a
    // full mode set the driver is entitled to reject on its own terms.
    // DM_POSITION alone asks only for the move.
    static const Plan kPlans[] = {
        {L"new primary first, position only", true, true},
        {L"new primary first, full mode", true, false},
        {L"snapshot order, full mode", false, false},
    };

    std::string lastError = "no attempt was made";

    const auto stage = [&](const SavedDisplay& display, const Plan& plan, LONG planShiftX,
                           LONG planShiftY) -> bool {
        const bool isTarget = display.deviceName == deviceName;

        // Re-read rather than trusting the snapshot: for the virtual display the
        // snapshot may predate the driver settling on the mode Attach asked for,
        // and staging a mode a display is not actually in is a mode change, not
        // a move.
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsExW(display.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) {
            mode = display.mode;
            mode.dmSize = sizeof(mode);
        }

        // Positions come from the snapshot this plan measured, not from the
        // re-read, so the translation is computed against one consistent
        // arrangement.
        mode.dmPosition.x = display.mode.dmPosition.x + planShiftX;
        mode.dmPosition.y = display.mode.dmPosition.y + planShiftY;
        mode.dmFields = plan.positionOnly ? DM_POSITION : (mode.dmFields | DM_POSITION);

        DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET;
        if (isTarget) flags |= CDS_SET_PRIMARY;

        const LONG staged =
            ChangeDisplaySettingsExW(display.deviceName.c_str(), &mode, nullptr, flags, nullptr);
        if (staged == DISP_CHANGE_SUCCESSFUL) return true;

        wprintf(L"    %s refused: %s (%ld); asked for %s with %s\n", display.deviceName.c_str(),
                ChangeResultName(staged), staged, DescribeMode(mode).c_str(),
                DescribeFlags(flags).c_str());
        lastError = Narrow(display.deviceName) + " refused to be repositioned (" +
                    Narrow(ChangeResultName(staged)) + ", code " + std::to_string(staged) + ")";
        return false;
    };

    for (const auto& plan : kPlans) {
        // Re-measure before every plan rather than reusing the entry snapshot.
        // A previous plan may have had to restore the desktop to get out of a
        // half-staged state, and Windows attaches and detaches displays on its
        // own besides - this machine drops the second adapter's panel routinely.
        // Every position staged below is absolute, so computing them from an
        // arrangement that is no longer on screen would scatter the displays.
        const auto current = Snapshot();
        const SavedDisplay* here = nullptr;
        for (const auto& display : current) {
            if (display.deviceName == deviceName) here = &display;
        }
        if (!here) {
            lastError = Narrow(deviceName) + " is no longer attached";
            wprintf(L"  %s went away; giving up\n", deviceName.c_str());
            break;
        }

        const LONG planShiftX = -here->mode.dmPosition.x;
        const LONG planShiftY = -here->mode.dmPosition.y;
        wprintf(L"  trying: %s (translating by (%ld,%ld))\n", plan.description, planShiftX,
                planShiftY);

        bool staged = true;
        if (plan.targetFirst) staged = stage(*here, plan, planShiftX, planShiftY);
        if (staged) {
            for (const auto& display : current) {
                if (plan.targetFirst && display.deviceName == deviceName) continue;
                staged = stage(display, plan, planShiftX, planShiftY);
                if (!staged) break;
            }
        }

        if (!staged) {
            // Nothing has reached the screen - every call above carried
            // CDS_NORESET - but the pending arrangement is now part this plan's
            // and part the desktop's, and on this hardware one refused staging
            // call makes the ones after it fail too. So it cannot be cleaned up
            // by staging the old values back and committing: those calls are
            // refused for the same reason, their failures are the thing being
            // recovered from, and the commit would then put the half-staged
            // arrangement on screen for real. That is not hypothetical - an
            // earlier version of this did exactly that and moved a 120Hz panel
            // to a driver-default position at 60Hz.
            //
            // Restore checks every staging result, reads the desktop back and
            // escalates to RestoreDefaultTopology when it does not match, so it
            // is the only thing here entitled to commit. Hand it the problem.
            wprintf(L"    undoing the half-staged arrangement\n");
            if (!Restore(current)) {
                lastError = "a plan was refused and the arrangement could not be put back";
                wprintf(L"    could not put it back; not trying anything further\n");
                break;
            }
            continue;
        }

        const LONG applied = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        if (applied != DISP_CHANGE_SUCCESSFUL) {
            wprintf(L"    applying the arrangement failed: %s (%ld)\n", ChangeResultName(applied),
                    applied);
            lastError = "applying the new arrangement was rejected (" +
                        Narrow(ChangeResultName(applied)) + ", code " + std::to_string(applied) +
                        ")";
            // The apply may have moved some displays, so the snapshot positions
            // the remaining plans are built from are no longer what is on screen
            // and stacking another translation on top would compound the error.
            break;
        }

        // Verify rather than trust: a game that ends up on the wrong display is
        // the whole failure this exists to prevent.
        if (verify()) {
            wprintf(L"  %s is primary at the origin (%s)\n", deviceName.c_str(), plan.description);
            return true;
        }
        wprintf(L"    applied, but Windows did not mark %s primary\n", deviceName.c_str());
        lastError = "Windows applied the change without making " + Narrow(deviceName) + " primary";
        break;
    }

    if (error) {
        *error = "every way of moving the origin was refused; ChangeDisplaySettingsEx: " +
                 lastError + "; SetDisplayConfig: " + ccdError;
    }
    return false;
}

bool DisplayTopology::CurrentMode(const std::wstring& deviceName, int* width, int* height) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) return false;
    if (width) *width = static_cast<int>(mode.dmPelsWidth);
    if (height) *height = static_cast<int>(mode.dmPelsHeight);
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
            if (error) *error = "Windows kept " + Narrow(display.deviceName) +
                                " attached despite reporting success";
            return false;
        }
    }
    return true;
}

DisplayTopology::RestoreOutcome DisplayTopology::TryRestore(
    const std::vector<SavedDisplay>& snapshot) {
    if (snapshot.empty()) return RestoreOutcome::Missing;

    // Primary first, so the others have a coordinate space to be placed around.
    bool stagedAll = true;
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& display : snapshot) {
            if (display.primary != (pass == 0)) continue;

            DEVMODEW mode = display.mode;
            mode.dmSize = sizeof(mode);
            mode.dmFields |= DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;

            DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET;
            if (display.primary) flags |= CDS_SET_PRIMARY;

            const LONG staged =
                ChangeDisplaySettingsExW(display.deviceName.c_str(), &mode, nullptr, flags, nullptr);
            if (staged != DISP_CHANGE_SUCCESSFUL) {
                wprintf(L"Restoring %s was refused: %s (%ld)\n", display.deviceName.c_str(),
                        ChangeResultName(staged), staged);
                stagedAll = false;
            }
        }
    }

    // Applied even when some staging was refused: this is the path back, and a
    // partial restore beats none. What makes that safe is the readback below -
    // unlike the outbound paths, nothing here trusts a return code.
    const LONG applied = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (applied != DISP_CHANGE_SUCCESSFUL) {
        wprintf(L"Applying the restore was refused: %s (%ld)\n", ChangeResultName(applied), applied);
    }

    // Verify rather than trust the return code. MakePrimary now genuinely moves
    // the desktop on every session, so this runs with real work to undo - a 4K
    // panel to move back off a negative origin, and displays to re-attach - and
    // a restore that quietly did none of it leaves the user with a rearranged
    // desktop and no indication why.
    //
    // The staging and apply codes are not consulted. They were only ever a
    // proxy for "did it work", the readback answers that directly, and on this
    // hardware they disagree in both directions: individual stagings are
    // refused and the desktop still comes back correct.
    (void)stagedAll;
    (void)applied;
    return CompareToSnapshot(snapshot);
}

bool DisplayTopology::Restore(const std::vector<SavedDisplay>& snapshot) {
    const RestoreOutcome outcome = TryRestore(snapshot);
    if (outcome == RestoreOutcome::Restored) return true;

    // Escalate ONLY for a display that did not come back at all. That is the
    // one kind of mismatch RestoreDefaultTopology can address: it is
    // SDC_TOPOLOGY_EXTEND / SDC_TOPOLOGY_INTERNAL, which choose which displays
    // are active and set neither positions nor primary.
    //
    // Escalating on a mispositioned display was useless by construction - it
    // fired seven times in one session of testing and corrected nothing, each
    // time logging the identical mismatch straight afterwards - and worse than
    // useless in general: on hardware where EXTEND does rearrange things, it
    // leaves an arrangement that is neither the snapshot nor what was there,
    // for the next attempt to restore against. The remedy for drift is the
    // caller's cheap one - drop the virtual display and restore again - so
    // leave that to it and report the failure.
    if (outcome != RestoreOutcome::Missing) {
        wprintf(L"The displays are not exactly where they were.\n");
        return false;
    }

    wprintf(L"A display did not come back; falling back to the default topology.\n");
    if (!RestoreDefaultTopology()) return false;
    // Report honestly: the desktop is usable, but it is not what was saved.
    return CompareToSnapshot(snapshot) == RestoreOutcome::Restored;
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
