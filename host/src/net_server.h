#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"
#include "window_list.h"

namespace thorstream {

struct StartRequest {
    uint64_t windowId = 0;
    int width = 0;  // 0 = native
    int height = 0;
    int fps = 60;
    int bitrateKbps = 20000;
    protocol::Codec codec = protocol::Codec::H264;
    uint16_t clientUdpPort = protocol::kDefaultVideoPort;
};

// TCP control channel + UDP video sender. Handles one client at a time, which is
// the whole point: this streams a game to a handheld, not to an audience.
class StreamServer {
public:
    struct Callbacks {
        std::function<std::vector<WindowEntry>()> listWindows;
        // Return false and fill `error` to reject; on success fill the actual
        // encoded size and the codec sequence header.
        std::function<bool(const StartRequest&, int* outWidth, int* outHeight,
                           std::vector<uint8_t>* outSequenceHeader, std::string* error)>
            startSession;
        std::function<void()> stopSession;
        std::function<void(const protocol::GamepadState&)> onGamepad;
        std::function<void()> onRequestIdr;
    };

    ~StreamServer();

    bool Start(uint16_t controlPort, Callbacks callbacks, std::string* error);
    void Stop();

    // Fragments and sends one encoded frame. Safe to call from the encoder thread.
    void SendVideoFrame(const uint8_t* data, size_t size, uint64_t timestampMicros,
                        bool keyframe);

    void SendError(const std::string& message);

    bool HasClient() const { return clientConnected_; }
    bool IsStreaming() const { return streaming_; }

private:
    void AcceptLoop();
    void ClientLoop(uintptr_t clientSocket);
    void HandleMessage(protocol::MessageType type, const uint8_t* payload, size_t size);

    void SendMessage(protocol::MessageType type, const protocol::Writer& payload);
    void SendWindowList();
    void HandleStart(protocol::Reader& reader);

    void CloseClient();

    Callbacks callbacks_;

    uintptr_t listenSocket_ = ~uintptr_t{0};
    uintptr_t clientSocket_ = ~uintptr_t{0};
    uintptr_t videoSocket_ = ~uintptr_t{0};

    std::thread acceptThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> clientConnected_{false};
    std::atomic<bool> streaming_{false};

    std::mutex sendMutex_;   // guards clientSocket_ writes
    std::mutex videoMutex_;  // guards videoSocket_ and the destination address

    std::vector<uint8_t> videoDestination_;  // sockaddr_in, opaque here
    std::atomic<uint32_t> frameNumber_{0};
};

}  // namespace thorstream
