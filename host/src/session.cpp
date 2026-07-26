#include "session.h"

#include <chrono>
#include <cstdio>

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
    callbacks.startSession = [this](const StartRequest& request, int* w, int* h,
                                    std::vector<uint8_t>* header, std::string* err) {
        return StartSession(request, w, h, header, err);
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
}

bool Session::StartSession(const StartRequest& request, int* outWidth, int* outHeight,
                           std::vector<uint8_t>* outSequenceHeader, std::string* error) {
    StopSession();

    HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(request.windowId));
    if (!IsWindow(hwnd)) {
        *error = "that window no longer exists - refresh the list";
        return false;
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
    int width = request.width > 0 ? request.width : client.right - client.left;
    int height = request.height > 0 ? request.height : client.bottom - client.top;

    // H.264 requires even dimensions.
    width &= ~1;
    height &= ~1;
    if (width <= 0 || height <= 0) {
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

    *outWidth = width;
    *outHeight = height;
    *outSequenceHeader = encoder_->SequenceHeader();
    return true;
}

void Session::StopSession() {
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
        encoder_->RepeatLastFrame(now, [this](const EncodedPacket& packet) {
            server_.SendVideoFrame(packet.data, packet.size, packet.timestamp, packet.isKeyframe);
            bytesSent_.fetch_add(packet.size);
        });
        lastFrameTicks_ = now;
    }
}

}  // namespace thorstream
