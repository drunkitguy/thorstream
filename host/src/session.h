#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "d3d_device.h"
#include "display_topology.h"
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

    // Read by the web console. All of these are atomics or atomics behind the
    // stream server, so they are safe to sample from its thread.
    bool HasClient() const { return server_.HasClient(); }
    bool IsStreaming() const { return server_.IsStreaming(); }
    uint64_t FramesSent() const { return framesSent_.load(); }
    uint64_t BytesSent() const { return bytesSent_.load(); }

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

private:
    // `displayPrepared` says the virtual display was already set up by the
    // caller, which is what the launch path does - before the game starts, so
    // the game itself picks the client's resolution. It stops this from
    // creating a second display, and stops the teardown of the previous session
    // from undoing the one that is already in place.
    bool StartSession(const StartRequest& request, int* outWidth, int* outHeight,
                      std::vector<uint8_t>* outSequenceHeader, std::string* error,
                      bool displayPrepared = false);

    // Launches a Playnite game, waits for its window, then streams it.
    bool LaunchAndStream(const LaunchRequest& request, int* outWidth, int* outHeight,
                         std::vector<uint8_t>* outSequenceHeader, std::string* error);

    // The body of LaunchAndStream, which only exists separately so that the one
    // caller can restore the displays if anything in it throws.
    bool LaunchAndStreamInner(const LaunchRequest& request, int* outWidth, int* outHeight,
                              std::vector<uint8_t>* outSequenceHeader, std::string* error);

    // Games can take a very long time to show a window: shader compilation,
    // launcher updates, anti-cheat startup.
    static constexpr int kLaunchTimeoutSeconds = 180;

    // The session reconnect is quick when it works at all, so a long wait here
    // would only delay telling the user that it did not.
    static constexpr int kUnlockTimeoutSeconds = 15;

    void StopSession();

    // Ends the capture/encode pipeline but leaves the display topology alone,
    // so a session can start onto a display that is already prepared.
    void StopStreaming();

    void OnCapturedFrame(const CapturedFrame& frame);

    // Windows.Graphics.Capture only produces a frame when the window changes.
    // A menu screen or a paused game would otherwise leave the client's decoder
    // with nothing to do and no way to tell the difference from a dead link.
    void KeepaliveLoop();

    GraphicsDevice& device_;
    StreamServer server_;

    // Creates the virtual display, attaches it and makes it primary - enough for
    // a game started afterwards to pick the client's resolution, while the
    // user's own monitors stay usable. Best effort: on any failure everything is
    // put back and the session carries on without a virtual display.
    bool PrepareVirtualDisplay(int width, int height, int fps);

    // Takes the physical displays away, leaving only the virtual one. Deliberately
    // separate from PrepareVirtualDisplay: on the launch path this must not happen
    // until the game's window exists, because until then the user is looking at a
    // launcher, a login prompt or a UAC dialog on their own screen.
    bool DetachPhysicalDisplays();

    // Puts the displays and the captured window back exactly as they were.
    // Safe to call repeatedly; does nothing if nothing was changed.
    void RestoreDisplays();

    std::mutex mutex_;
    std::unique_ptr<WindowCapture> capture_;
    std::unique_ptr<NvencEncoder> encoder_;

    std::unique_ptr<VirtualDisplay> virtualDisplay_;
    std::vector<SavedDisplay> savedDisplays_;
    // The desktop arrangement has been altered - attached, repositioned, made
    // primary, or detached - and owes the user a restore. Set before the
    // physical displays go away, because rearranging them is already a change
    // worth undoing.
    bool topologyChanged_ = false;
    bool displaysDetached_ = false;

    // The mode the virtual display is really in, read back rather than assumed:
    // both Attach and KeepOnly leave the final choice to Windows.
    int displayWidth_ = 0;
    int displayHeight_ = 0;

    // Bumped by anything that ends a session. A launch claims a value the moment
    // it starts and abandons its wait as soon as the counter moves past it, so a
    // stop or a shutdown never has to sit out the launch timeout on a thread it
    // has no other way to reach. A counter rather than a flag because a flag has
    // to be cleared, and the clear races with a bump from another thread.
    std::atomic<uint64_t> launchEpoch_{0};

    // The captured window is moved onto the virtual display, so remember where
    // it was in order to put it back.
    HWND capturedWindow_ = nullptr;
    WINDOWPLACEMENT savedPlacement_{};
    bool savedPlacementValid_ = false;

    std::thread keepaliveThread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> lastFrameTicks_{0};
    std::atomic<uint64_t> framesSent_{0};
    std::atomic<uint64_t> bytesSent_{0};
};

}  // namespace thorstream
