#include "session.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "scaler.h"
#include "window_list.h"

namespace thorstream {
namespace {

// If the window has not changed for this long, resend the last frame so the
// client's decoder keeps producing output and the link stays provably alive.
constexpr auto kKeepaliveInterval = std::chrono::milliseconds(200);

uint64_t NowMicros() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
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
        return StartSession(request, w, h, header, err);
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
    if (running_.exchange(false)) {
        if (keepaliveThread_.joinable()) keepaliveThread_.join();
    }
    server_.Stop();
    StopSession();
    // StopSession already restores, but a session that never started still needs
    // the marker cleared, and restoring twice is harmless.
    RestoreDisplays();
}

bool Session::StartSession(const StartRequest& request, int* outWidth, int* outHeight,
                           std::vector<uint8_t>* outSequenceHeader, std::string* error) {
    StopSession();

    HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(request.windowId));
    if (!IsWindow(hwnd)) {
        *error = "that window no longer exists - refresh the list";
        return false;
    }

    // Give the game a display of its own before capturing, so it renders at the
    // handheld's resolution rather than the desktop's. Best effort: if any part
    // of this fails we stream the window as-is rather than abandoning the
    // session, but we never leave displays detached on a failure.
    if (useVirtualDisplay && request.width > 0 && request.height > 0) {
        VirtualDisplayRequest displayRequest;
        displayRequest.width = request.width;
        displayRequest.height = request.height;
        displayRequest.refreshRate = request.fps > 0 ? request.fps : 60;

        std::string displayError;
        virtualDisplay_ = VirtualDisplay::Create(displayRequest, &displayError);
        if (!virtualDisplay_) {
            wprintf(L"Virtual display unavailable (%hs); streaming the window as-is.\n",
                    displayError.c_str());
        } else {
            wprintf(L"Virtual display %s created at %dx%d\n",
                    virtualDisplay_->DeviceName().c_str(), displayRequest.width,
                    displayRequest.height);

            savedDisplays_ = DisplayTopology::Snapshot();
            // Written before the change, so a crash mid-switch is recoverable.
            DisplayTopology::MarkDetached(savedDisplays_);

            std::string topologyError;
            // The monitor exists but is not part of the desktop until given a
            // mode, and a display that is not attached cannot be the one we keep.
            DisplayTopology::Attach(virtualDisplay_->DeviceName(), displayRequest.width,
                                    displayRequest.height, displayRequest.refreshRate,
                                    &topologyError);

            if (DisplayTopology::KeepOnly(virtualDisplay_->DeviceName(), &topologyError)) {
                displaysDetached_ = true;
                wprintf(L"Physical displays detached for the session.\n");

                // Everything has been herded onto the virtual display; put the
                // captured window where the game can actually use it.
                capturedWindow_ = hwnd;
                savedPlacement_.length = sizeof(savedPlacement_);
                savedPlacementValid_ = GetWindowPlacement(hwnd, &savedPlacement_) != FALSE;

                ShowWindow(hwnd, SW_RESTORE);
                SetWindowPos(hwnd, HWND_TOP, 0, 0, displayRequest.width, displayRequest.height,
                             SWP_NOACTIVATE);
                SetForegroundWindow(hwnd);
            } else {
                wprintf(L"Could not detach displays (%hs); leaving them alone.\n",
                        topologyError.c_str());
                DisplayTopology::ClearMarker();
                savedDisplays_.clear();
            }
        }
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
    // Note what is already on screen, so the game's window can be told apart
    // from everything that was here before it.
    std::vector<HWND> existing;
    for (const auto& window : EnumerateCapturableWindows()) existing.push_back(window.hwnd);

    FILETIME launchTime{};
    GetSystemTimeAsFileTime(&launchTime);

    server_.SendLaunchProgress("Launching...");
    if (!GameLibrary::Launch(request.gameId, error)) return false;

    server_.SendLaunchProgress("Waiting for the game to start...");
    const HWND window = GameLibrary::WaitForNewGameWindow(launchTime, kLaunchTimeoutSeconds, existing);
    if (!window) {
        *error = "the game did not open a window within " + std::to_string(kLaunchTimeoutSeconds) +
                 " seconds";
        return false;
    }

    // Launchers often show a window before the game itself does; give the real
    // one a moment to settle so we do not capture a splash screen forever.
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

    return StartSession(start, outWidth, outHeight, outSequenceHeader, error);
}

void Session::RestoreDisplays() {
    if (displaysDetached_) {
        wprintf(L"Restoring physical displays...\n");
        DisplayTopology::Restore(savedDisplays_);
        displaysDetached_ = false;
    }
    DisplayTopology::ClearMarker();
    savedDisplays_.clear();

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
    // Before tearing anything down: a client that vanished mid-press would
    // otherwise leave the virtual pad holding those inputs indefinitely.
    if (onReleaseInput) onReleaseInput();

    RestoreDisplays();

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
