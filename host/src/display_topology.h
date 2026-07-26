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

    // Leaves only `keepDeviceName` attached, and makes it primary.
    static bool KeepOnly(const std::wstring& keepDeviceName, std::string* error);

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
