#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "d3d_device.h"
#include "encoder_nvenc.h"
#include "net_server.h"
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

private:
    bool StartSession(const StartRequest& request, int* outWidth, int* outHeight,
                      std::vector<uint8_t>* outSequenceHeader, std::string* error);
    void StopSession();
    void OnCapturedFrame(const CapturedFrame& frame);

    // Windows.Graphics.Capture only produces a frame when the window changes.
    // A menu screen or a paused game would otherwise leave the client's decoder
    // with nothing to do and no way to tell the difference from a dead link.
    void KeepaliveLoop();

    GraphicsDevice& device_;
    StreamServer server_;

    std::mutex mutex_;
    std::unique_ptr<WindowCapture> capture_;
    std::unique_ptr<NvencEncoder> encoder_;

    std::thread keepaliveThread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> lastFrameTicks_{0};
    std::atomic<uint64_t> framesSent_{0};
    std::atomic<uint64_t> bytesSent_{0};
};

}  // namespace thorstream
