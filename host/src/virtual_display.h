#pragma once

#include <Windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "display_topology.h"

namespace thorstream {

struct VirtualDisplayRequest {
    int width = 1920;
    int height = 1080;
    int refreshRate = 60;
    std::string clientName = "thorstream";
};

// Creates a virtual monitor sized for the client, so the game renders at the
// handheld's resolution instead of the PC's and we avoid a downscale entirely.
//
// The driver runs a 3-second watchdog: stop pinging and the display disappears
// by itself. That is a safety feature, not an obstacle - if this process dies
// while physical displays are detached, the virtual one goes with it and Windows
// is forced to bring a real display back.
class VirtualDisplay {
public:
    ~VirtualDisplay();

    VirtualDisplay(const VirtualDisplay&) = delete;
    VirtualDisplay& operator=(const VirtualDisplay&) = delete;

    static bool IsDriverAvailable();

    // Returns nullptr with `error` set if the driver is missing or refuses.
    static std::unique_ptr<VirtualDisplay> Create(const VirtualDisplayRequest& request,
                                                  std::string* error);

    // GDI device name, e.g. "\\\\.\\DISPLAY3". Empty if Windows never surfaced it.
    const std::wstring& DeviceName() const { return deviceName_; }
    int Width() const { return request_.width; }
    int Height() const { return request_.height; }

private:
    VirtualDisplay();

    void PingLoop();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    VirtualDisplayRequest request_;
    std::wstring deviceName_;

    std::thread pingThread_;
    std::atomic<bool> running_{false};
};

// Puts `snapshot` back, removing `display` if that is what stands in the way.
//
// Two attempts, in this order for a reason. The first runs with the virtual
// display still attached, so there is never a moment with nothing at all
// displaying - the single worst outcome this code has. If that does not
// reproduce the snapshot, the virtual display is almost always why: MakePrimary
// left it at the origin, which is the coordinate a physical display is trying
// to return to, and Windows will not hand the same coordinates to two displays.
// Measured: with it still up, everything comes back shifted by exactly its
// width; dropping it and restoring again fixes that every time.
//
// The order is cheap-and-targeted first, destructive-and-untargeted last: only
// the second attempt is allowed to escalate to RestoreDefaultTopology, and only
// then for a display that is missing outright.
//
// One function rather than one per caller: the session teardown and the
// diagnostic modes need identical behaviour, and two copies of this had already
// drifted apart within minutes of being written.
bool RestoreDisplaysAround(const std::vector<SavedDisplay>& snapshot,
                           std::unique_ptr<VirtualDisplay>& display);

}  // namespace thorstream
