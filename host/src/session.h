#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "d3d_device.h"
#include "display_topology.h"
#include "popup_overlay.h"
#include "encoder_nvenc.h"
#include "net_server.h"
#include "virtual_display.h"
#include "window_capture.h"

namespace thorstream {

// Owns the capture -> encode -> send pipeline for one streaming session, and
// wires it to the control server.
class Session {
public:
    explicit Session(GraphicsDevice& device);
    ~Session();

    bool Serve(uint16_t controlPort, std::string* error);
    void Shutdown();

    // Set by main once ViGEm is available; may be null.
    std::function<void(const protocol::GamepadState&)> onGamepad;
    // Invoked when a session ends, so held buttons can be released.
    std::function<void()> onReleaseInput;

    // Encode full-range rather than studio-range colour. More accurate, but
    // relies on the client honouring the range flag we put in the bitstream.
    bool fullRange = false;

    // Give the game its own display at the client's resolution and detach the
    // physical ones for the duration of the session.
    bool useVirtualDisplay = true;

    // Draw dialogs that appear over the game into the stream. Loosens the
    // isolation slightly - a notification could now be visible - so it is a
    // choice rather than an assumption.
    bool forwardPopups = true;

private:
    bool StartSession(const StartRequest& request, int* outWidth, int* outHeight,
                      std::vector<uint8_t>* outSequenceHeader, std::string* error);

    // Launches a Playnite game, waits for its window, then streams it.
    bool LaunchAndStream(const LaunchRequest& request, int* outWidth, int* outHeight,
                         std::vector<uint8_t>* outSequenceHeader, std::string* error);

    // Games can take a very long time to show a window: shader compilation,
    // launcher updates, anti-cheat startup.
    static constexpr int kLaunchTimeoutSeconds = 180;
    void StopSession();
    void OnCapturedFrame(const CapturedFrame& frame);

    // Windows.Graphics.Capture only produces a frame when the window changes.
    // A menu screen or a paused game would otherwise leave the client's decoder
    // with nothing to do and no way to tell the difference from a dead link.
    void KeepaliveLoop();

    GraphicsDevice& device_;
    StreamServer server_;

    // Puts the displays and the captured window back exactly as they were.
    // Safe to call repeatedly; does nothing if nothing was changed.
    void RestoreDisplays();

    std::mutex mutex_;
    std::unique_ptr<WindowCapture> capture_;
    std::unique_ptr<NvencEncoder> encoder_;

    std::unique_ptr<PopupOverlay> popups_;
    std::unique_ptr<VirtualDisplay> virtualDisplay_;
    std::vector<SavedDisplay> savedDisplays_;
    bool displaysDetached_ = false;

    // The captured window is moved onto the virtual display, so remember where
    // it was in order to put it back.
    HWND capturedWindow_ = nullptr;
    WINDOWPLACEMENT savedPlacement_{};
    bool savedPlacementValid_ = false;
    bool loggedOverlay_ = false;

    // A copy of the last game frame, so popups can be composited even while the
    // game itself has stopped producing frames.
    winrt::com_ptr<ID3D11Texture2D> lastGameFrame_;
    RECT lastGameCrop_{};
    bool hasGameFrame_ = false;

    void RememberGameFrame(const CapturedFrame& frame);
    std::vector<NvencEncoder::Overlay> CollectOverlays();

    std::thread keepaliveThread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> lastFrameTicks_{0};
    std::atomic<uint64_t> framesSent_{0};
    std::atomic<uint64_t> bytesSent_{0};
};

}  // namespace thorstream
