#pragma once

#include <Windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

}  // namespace thorstream
