#include "session.h"

#include <dwmapi.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "scaler.h"
#include "session_lock.h"
#include "window_list.h"

namespace thorstream {
namespace {

// If the window has not changed for this long, resend the last frame so the
// client's decoder keeps producing output and the link stays provably alive.
constexpr auto kKeepaliveInterval = std::chrono::milliseconds(200);

// The requested size becomes a real display mode, and briefly the machine's
// only one, so it cannot be taken on trust from the wire. The floor is the
// smallest window WaitForNewGameWindow will accept, so anything under it could
// never produce a session anyway; the ceiling is 8K.
constexpr int kMinStreamWidth = 640;
constexpr int kMinStreamHeight = 480;
constexpr int kMaxStreamWidth = 7680;
constexpr int kMaxStreamHeight = 4320;

uint64_t NowMicros() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Zero means "native": no display is created, so there is nothing to check.
bool ValidateRequestedSize(int width, int height, std::string* error) {
    if (width == 0 && height == 0) return true;
    if (width < kMinStreamWidth || height < kMinStreamHeight || width > kMaxStreamWidth ||
        height > kMaxStreamHeight) {
        *error = "requested size " + std::to_string(width) + "x" + std::to_string(height) +
                 " is out of range (" + std::to_string(kMinStreamWidth) + "x" +
                 std::to_string(kMinStreamHeight) + " to " + std::to_string(kMaxStreamWidth) + "x" +
                 std::to_string(kMaxStreamHeight) + ")";
        return false;
    }
    return true;
}

}  // namespace

Session::Session(GraphicsDevice& device) : device_(device) {}

Session::~Session() {
    Shutdown();
}

bool Session::Serve(uint16_t controlPort, std::string* error) {
    StreamServer::Callbacks callbacks;
    callbacks.listWindows = [] { return EnumerateCapturableWindows(); };
    callbacks.listGames = [] {
        std::string error;
        auto games = GameLibrary::Read(&error);
        if (games.empty() && !error.empty()) {
            wprintf(L"Playnite library unavailable: %hs\n", error.c_str());
        }
        return games;
    };
    callbacks.startSession = [this](const StartRequest& request, int* w, int* h,
                                    std::vector<uint8_t>* header, std::string* err) {
        // This path detaches the physical displays too, and it runs on the
        // server's client thread: an exception escaping a thread entry point is
        // std::terminate, with the user's monitors gone. StartSession's own
        // catch only covers winrt::hresult_error.
        try {
            return StartSession(request, w, h, header, err);
        } catch (...) {
            RestoreDisplays();
            throw;
        }
    };
    callbacks.launchGame = [this](const LaunchRequest& request, int* w, int* h,
                                  std::vector<uint8_t>* header, std::string* err) {
        return LaunchAndStream(request, w, h, header, err);
    };
    callbacks.stopSession = [this] { StopSession(); };
    callbacks.onRequestIdr = [this] {
        std::lock_guard lock(mutex_);
        if (encoder_) encoder_->RequestKeyframe();
    };
    callbacks.onGamepad = [this](const protocol::GamepadState& state) {
        if (onGamepad) onGamepad(state);
    };

    if (!server_.Start(controlPort, std::move(callbacks), error)) return false;

    running_ = true;
    keepaliveThread_ = std::thread(&Session::KeepaliveLoop, this);
    return true;
}

void Session::Shutdown() {
    // A launch in progress owns the client thread for as long as the launch
    // timeout, and StreamServer::Stop below joins that thread. Let it out first.
    ++launchEpoch_;

    if (running_.exchange(false)) {
        if (keepaliveThread_.joinable()) keepaliveThread_.join();
    }
    server_.Stop();
    StopSession();
    // StopSession already restores, but a session that never started still needs
    // the marker cleared, and restoring twice is harmless.
    RestoreDisplays();
}

bool Session::PrepareVirtualDisplay(int width, int height, int fps) {
    VirtualDisplayRequest displayRequest;
    displayRequest.width = width;
    displayRequest.height = height;
    displayRequest.refreshRate = fps > 0 ? fps : 60;

    std::string displayError;
    virtualDisplay_ = VirtualDisplay::Create(displayRequest, &displayError);
    if (!virtualDisplay_) {
        wprintf(L"Virtual display unavailable (%hs); streaming the window as-is.\n",
                displayError.c_str());
        return false;
    }

    wprintf(L"Virtual display %s created at %dx%d\n", virtualDisplay_->DeviceName().c_str(),
            displayRequest.width, displayRequest.height);

    savedDisplays_ = DisplayTopology::Snapshot();
    // Written before the change, so a crash mid-switch is recoverable.
    DisplayTopology::MarkDetached(savedDisplays_);
    // From here the desktop owes the user a restore even if the steps below
    // fail: attaching and repositioning are changes in their own right.
    topologyChanged_ = true;

    std::string topologyError;
    // The monitor exists but is not part of the desktop until given a mode, and
    // a display that is not attached cannot be made primary or kept.
    if (!DisplayTopology::Attach(virtualDisplay_->DeviceName(), displayRequest.width,
                                 displayRequest.height, displayRequest.refreshRate,
                                 &topologyError)) {
        wprintf(L"Could not attach the virtual display (%hs); streaming the window as-is.\n",
                topologyError.c_str());
        RestoreDisplays();
        return false;
    }

    // Being primary is what makes a game choose this resolution: it is the
    // display at the origin, the one a fullscreen window lands on, and the one
    // enumerated first. That is all the game needs, and it needs it at startup -
    // so the physical displays can stay attached for now, which matters because
    // the game may take minutes to appear and the user has to be able to see
    // the launcher, the login prompt or the UAC dialog in the meantime.
    if (!DisplayTopology::MakePrimary(virtualDisplay_->DeviceName(), &topologyError)) {
        wprintf(L"Could not make the virtual display primary (%hs); streaming the window as-is.\n",
                topologyError.c_str());
        RestoreDisplays();
        return false;
    }

    // Windows picks the final mode, so read back what the game will actually
    // see rather than assuming the request was honoured.
    if (!DisplayTopology::CurrentMode(virtualDisplay_->DeviceName(), &displayWidth_,
                                      &displayHeight_)) {
        displayWidth_ = displayRequest.width;
        displayHeight_ = displayRequest.height;
    }
    wprintf(L"Virtual display is primary at %dx%d (requested %dx%d)\n", displayWidth_,
            displayHeight_, displayRequest.width, displayRequest.height);
    if (displayWidth_ != displayRequest.width || displayHeight_ != displayRequest.height) {
        wprintf(L"Windows did not honour the requested mode; the client will get a scaled frame.\n");
    }
    return true;
}

bool Session::DetachPhysicalDisplays() {
    if (!virtualDisplay_) return false;

    std::string topologyError;
    if (!DisplayTopology::KeepOnly(virtualDisplay_->DeviceName(), &topologyError)) {
        wprintf(L"Could not detach displays (%hs); leaving them alone.\n", topologyError.c_str());
        return false;
    }

    displaysDetached_ = true;
    wprintf(L"Physical displays detached for the session.\n");

    // KeepOnly hands Windows an invalid mode index together with
    // SDC_ALLOW_CHANGES, so Windows is free to pick a different mode for the one
    // remaining path. Read it back: everything downstream sizes off this.
    int actualWidth = 0, actualHeight = 0;
    if (DisplayTopology::CurrentMode(virtualDisplay_->DeviceName(), &actualWidth, &actualHeight) &&
        actualWidth > 0 && actualHeight > 0) {
        if (actualWidth != displayWidth_ || actualHeight != displayHeight_) {
            wprintf(L"Virtual display mode changed to %dx%d when the others were detached\n",
                    actualWidth, actualHeight);
        }
        displayWidth_ = actualWidth;
        displayHeight_ = actualHeight;
    }
    return true;
}

bool Session::StartSession(const StartRequest& request, int* outWidth, int* outHeight,
                           std::vector<uint8_t>* outSequenceHeader, std::string* error,
                           bool displayPrepared) {
    // Without a virtual display the requested size never becomes a display mode:
    // it is only an upper bound for the scaler, and a small one is a legitimate
    // ask - an Android client in split-screen reports its own window bounds, not
    // the panel's. So police it only when a display is about to be created, and
    // otherwise downscale as this path always has.
    const bool willCreateDisplay =
        !displayPrepared && useVirtualDisplay && request.width > 0 && request.height > 0;
    if (willCreateDisplay && !ValidateRequestedSize(request.width, request.height, error)) {
        return false;
    }

    // A full stop would put the physical displays back, which is exactly what
    // the launch path spent the last few minutes avoiding.
    if (displayPrepared) {
        StopStreaming();
    } else {
        StopSession();
    }

    HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(request.windowId));
    if (!IsWindow(hwnd)) {
        RestoreDisplays();
        *error = "that window no longer exists - refresh the list";
        return false;
    }

    // Give the window a display of its own before capturing. Nothing is being
    // launched here, so there is no wait to keep the user's monitors alive for:
    // prepare and detach in one go, as this path always has. Best effort - if
    // any part fails we stream the window as-is rather than abandoning the
    // session, and never leave the displays rearranged on a failure.
    if (willCreateDisplay && PrepareVirtualDisplay(request.width, request.height, request.fps)) {
        DetachPhysicalDisplays();
    }

    if (displaysDetached_ && displayWidth_ > 0 && displayHeight_ > 0) {
        // Everything has been herded onto the virtual display; put the captured
        // window where the game can actually use it. A game launched after the
        // switch is already sized for it, so this is usually a no-op - but a
        // window that predates the switch still needs moving.
        capturedWindow_ = hwnd;
        savedPlacement_.length = sizeof(savedPlacement_);
        savedPlacementValid_ = GetWindowPlacement(hwnd, &savedPlacement_) != FALSE;

        ShowWindow(hwnd, SW_RESTORE);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, displayWidth_, displayHeight_, SWP_NOACTIVATE);

        // SetWindowPos sizes the window frame, but it is the client area that is
        // captured and encoded, so a bordered window would otherwise land just
        // under the target size and be encoded there. Measure what we got and
        // add the difference back, which unlike AdjustWindowRectEx stays correct
        // for per-monitor-DPI windows and custom frames.
        RECT frame{}, area{};
        if (GetWindowRect(hwnd, &frame) && GetClientRect(hwnd, &area)) {
            // Measured against GetWindowRect on purpose: that is the rect
            // SetWindowPos sizes, so the chrome added back has to be in the same
            // space, invisible resize border included.
            const int extraWidth = (frame.right - frame.left) - (area.right - area.left);
            const int extraHeight = (frame.bottom - frame.top) - (area.bottom - area.top);

            // Placement is the other half. On Windows 10+ that invisible border
            // means a window at x=0 has its visible edge several pixels inside
            // the display (~11px per side at 150% DPI, as window_capture found),
            // and the same again hanging off the far edge. DWM reports the
            // visible bounds directly, so offset by the difference.
            RECT visible{};
            LONG insetX = 0, insetY = 0;
            if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &visible,
                                                sizeof(visible)))) {
                insetX = visible.left - frame.left;
                insetY = visible.top - frame.top;
            }

            if (extraWidth > 0 || extraHeight > 0 || insetX > 0 || insetY > 0) {
                SetWindowPos(hwnd, HWND_TOP, -insetX, -insetY, displayWidth_ + extraWidth,
                             displayHeight_ + extraHeight, SWP_NOACTIVATE);
            }
        }

        SetForegroundWindow(hwnd);
    }

    CaptureOptions options;
    options.cropToClientArea = true;
    options.captureCursor = false;
    options.drawBorder = false;
    options.maxFramesPerSecond = request.fps;

    std::lock_guard lock(mutex_);
    try {
        capture_ = std::make_unique<WindowCapture>(device_, hwnd, options);
    } catch (const winrt::hresult_error& e) {
        RestoreDisplays();
        *error = "could not capture that window (0x" + std::to_string(e.code().value) + ")";
        return false;
    }

    // The encoder needs a fixed size, so take it from the window's client area
    // rather than the raw capture surface, which includes any window chrome.
    RECT client{};
    GetClientRect(hwnd, &client);
    const int sourceWidth = client.right - client.left;
    const int sourceHeight = client.bottom - client.top;

    // The client's width/height are an upper bound, not an exact demand: it knows
    // its screen size, not the window's aspect ratio. Fitting here keeps a 21:9
    // window from being squashed onto a 16:9 panel.
    int width = 0, height = 0;
    FitPreservingAspect(sourceWidth, sourceHeight, request.width, request.height, &width, &height);

    if (sourceWidth <= 0 || sourceHeight <= 0) {
        capture_.reset();
        RestoreDisplays();
        *error = "the window has no usable client area (is it minimised?)";
        return false;
    }

    EncoderSettings settings;
    settings.width = width;
    settings.height = height;
    settings.framerate = request.fps;
    settings.bitrateKbps = request.bitrateKbps;
    settings.useHevc = (request.codec == protocol::Codec::Hevc);
    settings.fullRange = fullRange;

    std::string encoderError;
    encoder_ = NvencEncoder::Create(device_.device.get(), device_.context.get(), settings,
                                    &encoderError);
    if (!encoder_) {
        capture_.reset();
        RestoreDisplays();
        *error = "NVENC: " + encoderError;
        return false;
    }

    framesSent_ = 0;
    bytesSent_ = 0;
    lastFrameTicks_ = NowMicros();

    capture_->Start([this](const CapturedFrame& frame) { OnCapturedFrame(frame); },
                    [this] { server_.SendError("the captured window was closed"); });

    if (width != sourceWidth || height != sourceHeight) {
        wprintf(L"scaling %dx%d down to %dx%d for the client\n", sourceWidth, sourceHeight, width,
                height);
    }

    *outWidth = width;
    *outHeight = height;
    *outSequenceHeader = encoder_->SequenceHeader();
    return true;
}

bool Session::LaunchAndStream(const LaunchRequest& request, int* outWidth, int* outHeight,
                              std::vector<uint8_t>* outSequenceHeader, std::string* error) {
    // This runs on the server's client thread, and an exception escaping a
    // thread entry point is std::terminate - with the user's displays still
    // rearranged. Nothing here is expected to throw, but a three-minute wait
    // that allocates on every poll is exactly where std::bad_alloc would land.
    try {
        return LaunchAndStreamInner(request, outWidth, outHeight, outSequenceHeader, error);
    } catch (...) {
        RestoreDisplays();
        throw;
    }
}

bool Session::LaunchAndStreamInner(const LaunchRequest& request, int* outWidth, int* outHeight,
                                   std::vector<uint8_t>* outSequenceHeader, std::string* error) {
    // A size only needs policing when it is about to become a display mode.
    const bool useDisplay = useVirtualDisplay && request.width > 0 && request.height > 0;
    if (useDisplay && !ValidateRequestedSize(request.width, request.height, error)) return false;

    // Claim this launch's generation before anything slow happens. Not via
    // StopSession: that bumps the counter too, and clearing afterwards would
    // clobber a bump that a stop or a shutdown on another thread landed in the
    // meantime - which is seconds wide, since the teardown below waits on frame
    // callbacks and performs a real mode change.
    const uint64_t epoch = ++launchEpoch_;
    const auto abandoned = [this, epoch] { return launchEpoch_.load() != epoch; };

    // End any previous session first: the display switch below has to start from
    // the real topology, and StartSession will not do this for us once the
    // display is already prepared. This is StopSession minus the bump.
    StopStreaming();
    RestoreDisplays();

    // Note what is already on screen, so the game's window can be told apart
    // from everything that was here before it.
    std::vector<HWND> existing;
    for (const auto& window : EnumerateCapturableWindows()) existing.push_back(window.hwnd);

    // A locked session barely composites, so capture collapses to ~3 fps and the
    // game would be unplayable even though everything "worked". Unlock before
    // launching rather than after, so the game never opens onto a dead desktop.
    if (SessionLock::IsLocked()) {
        server_.SendLaunchProgress("Unlocking the PC...");
        std::string unlockError;
        if (!SessionLock::RequestUnlock(kUnlockTimeoutSeconds, &unlockError)) {
            *error = unlockError;
            return false;
        }
    }
    // Nothing can stop a deliberate lock, but this rules out the screensaver and
    // display-sleep paths for as long as the stream lasts.
    SessionLock::PreventLocking();

    // A game picks its resolution from the displays it finds when it starts, so
    // the virtual display has to exist and be primary before it launches. Doing
    // it afterwards only moves an already-4K window onto a 1080p display, which
    // costs a downscale and renders the game at four times the pixels the client
    // will ever see.
    //
    // The physical displays are deliberately NOT detached yet. The wait below
    // can last minutes - a launcher update, a login prompt, an EULA, a UAC
    // dialog the user has to answer - and it is not interruptible from the
    // network, so detaching here would leave them staring at dark monitors with
    // no way to fix it. They come off once the game is up.
    //
    // From here on the desktop is rearranged, so every exit path below has to
    // reach RestoreDisplays().
    bool displayPrepared = false;
    if (useDisplay) {
        // The client's loading screen keys its progress bar off these strings,
        // and the switch below takes seconds.
        server_.SendLaunchProgress("Preparing the display...");
        displayPrepared = PrepareVirtualDisplay(request.width, request.height, request.fps);
    }

    FILETIME launchTime{};
    GetSystemTimeAsFileTime(&launchTime);

    server_.SendLaunchProgress("Launching...");
    if (!GameLibrary::Launch(request.gameId, error)) {
        RestoreDisplays();
        return false;
    }

    server_.SendLaunchProgress("Waiting for the game to start...");
    const HWND window =
        GameLibrary::WaitForNewGameWindow(launchTime, kLaunchTimeoutSeconds, existing, abandoned);
    if (!window) {
        RestoreDisplays();
        // An already-running game is the common case here: Playnite just focuses
        // the window it already has, no new process starts, and nothing this
        // waits for ever happens.
        *error = abandoned() ? "the launch was cancelled"
                             : "the game did not open a window within " +
                                   std::to_string(kLaunchTimeoutSeconds) +
                                   " seconds (is it already running?)";
        return false;
    }

    // The game is up and has already sized itself to the virtual display, so the
    // physical ones can go now. Failing here is survivable - the game is still
    // rendering at the right size, it is just not alone on the desktop.
    if (displayPrepared) DetachPhysicalDisplays();

    // Launchers often show a window before the game itself does; give the real
    // one a moment to settle so we do not capture a splash screen forever. It
    // also gives the game a moment to react to the displays that just vanished.
    server_.SendLaunchProgress("Starting the stream...");
    std::this_thread::sleep_for(std::chrono::seconds(2));

    StartRequest start;
    start.windowId = reinterpret_cast<uint64_t>(window);
    start.width = request.width;
    start.height = request.height;
    start.fps = request.fps;
    start.bitrateKbps = request.bitrateKbps;
    start.codec = request.codec;
    start.clientUdpPort = request.clientUdpPort;

    return StartSession(start, outWidth, outHeight, outSequenceHeader, error, displayPrepared);
}

void Session::RestoreDisplays() {
    // Not gated on displaysDetached_: making the virtual display primary moves
    // every other display, which needs putting back even if none were detached.
    if (topologyChanged_) {
        wprintf(L"Restoring physical displays...\n");
        DisplayTopology::Restore(savedDisplays_);
        topologyChanged_ = false;
        displaysDetached_ = false;
    }
    DisplayTopology::ClearMarker();
    savedDisplays_.clear();
    displayWidth_ = 0;
    displayHeight_ = 0;

    // Drop the virtual display only after the real ones are back, so the desktop
    // is never left with nothing at all.
    virtualDisplay_.reset();

    if (capturedWindow_ && savedPlacementValid_ && IsWindow(capturedWindow_)) {
        SetWindowPlacement(capturedWindow_, &savedPlacement_);
    }
    capturedWindow_ = nullptr;
    savedPlacementValid_ = false;
}

void Session::StopSession() {
    // A launch still waiting for a game window cannot hear a stop any other way:
    // it is holding the very thread that would have read it off the socket.
    ++launchEpoch_;

    // Capture first: tearing the virtual display down underneath a running
    // capture would report the window as closed and send the client an error on
    // the way out.
    StopStreaming();
    RestoreDisplays();
}

void Session::StopStreaming() {
    // Before tearing anything down: a client that vanished mid-press would
    // otherwise leave the virtual pad holding those inputs indefinitely.
    if (onReleaseInput) onReleaseInput();

    std::unique_ptr<WindowCapture> capture;
    std::unique_ptr<NvencEncoder> encoder;
    {
        std::lock_guard lock(mutex_);
        capture = std::move(capture_);
        encoder = std::move(encoder_);
    }
    // Destroyed outside the lock: WindowCapture::Stop waits for in-flight frame
    // callbacks, which themselves take the lock.
    capture.reset();
    encoder.reset();
}

void Session::OnCapturedFrame(const CapturedFrame& frame) {
    std::lock_guard lock(mutex_);
    if (!encoder_) return;

    encoder_->EncodeFrame(frame.texture, frame.crop, NowMicros(),
                          [this](const EncodedPacket& packet) {
                              server_.SendVideoFrame(packet.data, packet.size, packet.timestamp,
                                                     packet.isKeyframe);
                              framesSent_.fetch_add(1);
                              bytesSent_.fetch_add(packet.size);
                          });
    lastFrameTicks_ = NowMicros();
}

void Session::KeepaliveLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!server_.IsStreaming()) continue;

        const uint64_t now = NowMicros();
        const uint64_t idleMicros = now - lastFrameTicks_.load();
        if (idleMicros < static_cast<uint64_t>(kKeepaliveInterval.count()) * 1000) continue;

        std::lock_guard lock(mutex_);
        if (!encoder_) continue;

        const auto onPacket = [this](const EncodedPacket& packet) {
            server_.SendVideoFrame(packet.data, packet.size, packet.timestamp, packet.isKeyframe);
            bytesSent_.fetch_add(packet.size);
        };

        encoder_->RepeatLastFrame(now, onPacket);
        lastFrameTicks_ = now;
    }
}

}  // namespace thorstream
