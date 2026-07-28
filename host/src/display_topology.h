#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace thorstream {

struct SavedDisplay {
    std::wstring deviceName;  // \\.\DISPLAY1
    DEVMODEW mode{};
    bool primary = false;
};

// Detaches every physical display so the streamed game has the virtual display
// to itself, and puts them back afterwards.
//
// This is the most dangerous thing the host does: a failure here leaves a
// machine with no usable screen. Every path that can end a session restores, a
// snapshot is written to disk so a crash can be recovered from on next launch,
// and the virtual display driver's own 3-second watchdog acts as the final
// backstop - if this process dies, the virtual display dies with it and Windows
// is forced to bring a physical display back.
class DisplayTopology {
public:
    // Everything currently attached, so it can be restored exactly.
    static std::vector<SavedDisplay> Snapshot();

    // A newly created virtual monitor exists as a device but is not part of the
    // desktop until it is given a mode. Until then it cannot be kept, only
    // detached along with everything else.
    static bool Attach(const std::wstring& deviceName, int width, int height, int refreshRate,
                       std::string* error);

    // Moves the origin onto `deviceName`, which is what "primary" means to
    // Windows and to a game choosing a display. Every other display is shifted
    // by the same offset, so the arrangement is preserved.
    //
    // Returning false does NOT mean the desktop was left alone. Windows can
    // accept a rearrangement and then not mark the display primary, and it can
    // fail an apply partway; both leave displays moved. The caller owes the user
    // a Restore on failure just as much as on success.
    static bool MakePrimary(const std::wstring& deviceName, std::string* error);

    // Leaves only `keepDeviceName` attached, and makes it primary.
    static bool KeepOnly(const std::wstring& keepDeviceName, std::string* error);

    // The mode a display is actually in. Both Attach and KeepOnly let Windows
    // pick a mode, so the size asked for is not necessarily the size in use.
    static bool CurrentMode(const std::wstring& deviceName, int* width, int* height);

    // How close the desktop ended up to the snapshot. The distinction is what
    // decides whether escalating to RestoreDefaultTopology could possibly help:
    // that call chooses which displays are active and nothing else, so it can
    // address Missing and can do nothing at all about Drifted.
    enum class RestoreOutcome {
        Restored,  // exactly as saved
        Drifted,   // everything is back, but not all where or how it was
        Missing,   // at least one saved display is not attached
    };

    // Puts the arrangement back and reports what actually happened, without
    // taking any further action. Callers that can remove whatever is standing
    // in the way - the virtual display, usually - should use this, deal with
    // it, and try again before resorting to Restore.
    static RestoreOutcome TryRestore(const std::vector<SavedDisplay>& snapshot);

    // TryRestore, escalating to RestoreDefaultTopology if and only if a display
    // did not come back at all. Returns whether the desktop matches `snapshot`.
    static bool Restore(const std::vector<SavedDisplay>& snapshot);

    // Last-ditch recovery when an exact restore is impossible - after a crash,
    // or across a reboot where adapter ids are no longer the same.
    static bool RestoreDefaultTopology();

    // A marker on disk saying "displays were detached and not put back". Written
    // before touching anything, cleared after a successful restore, and checked
    // at startup.
    static void MarkDetached(const std::vector<SavedDisplay>& snapshot);
    static void ClearMarker();
    static bool RecoverIfMarked();
};

}  // namespace thorstream
