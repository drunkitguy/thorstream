#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <winrt/base.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "d3d_device.h"
#include "window_capture.h"

namespace thorstream {

// A popup's latest content, ready to be drawn over the game frame.
struct PopupSnapshot {
    ID3D11Texture2D* texture = nullptr;
    RECT source{};   // region of `texture` holding the popup
    RECT onScreen{}; // where it sits, in screen coordinates
};

// Dialogs that appear over the game are separate windows, so capturing only the
// game means they never reach the stream - you get a game that has silently
// stopped responding because something is asking a question you cannot see.
//
// Each qualifying popup is captured as its own window and composited over the
// game frame. That keeps the project's premise intact: we still only ever
// capture windows we have explicitly chosen, rather than falling back to
// capturing the whole desktop.
//
// The trade-off is real and worth stating: a notification containing something
// private can now reach the stream, where previously nothing outside the game
// window could. Hence the toggle.
class PopupOverlay {
public:
    PopupOverlay(GraphicsDevice& device, HWND gameWindow);
    ~PopupOverlay();

    PopupOverlay(const PopupOverlay&) = delete;
    PopupOverlay& operator=(const PopupOverlay&) = delete;

    void Start();
    void Stop();

    // Snapshots of every popup currently on screen. Textures stay valid until
    // the next call, so compositing must happen before then - which it does,
    // since both run on the capture thread.
    std::vector<PopupSnapshot> Current();

private:
    struct Tracked;

    void ScanLoop();
    bool Qualifies(HWND window) const;

    GraphicsDevice& device_;
    HWND gameWindow_;

    std::mutex mutex_;
    std::vector<std::unique_ptr<Tracked>> tracked_;

    std::thread scanThread_;
    std::atomic<bool> running_{false};
};

}  // namespace thorstream
