#include "net_server.h"

#include <WinSock2.h>
#include <ws2tcpip.h>

#include <cstdio>

#include "cover_art.h"
#include "input_injector.h"

#pragma comment(lib, "Ws2_32.lib")

namespace thorstream {
namespace {

constexpr uintptr_t kInvalid = ~uintptr_t{0};

std::string Utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

bool EnsureWinsock(std::string* error) {
    static bool initialised = false;
    static bool ok = false;
    if (initialised) {
        if (!ok && error) *error = "WSAStartup failed";
        return ok;
    }
    initialised = true;
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    ok = (result == 0);
    if (!ok && error) *error = "WSAStartup failed: " + std::to_string(result);
    return ok;
}

// Reads exactly `size` bytes, or fails. Returns false on close/error.
bool RecvExact(SOCKET socket, void* buffer, size_t size) {
    auto* out = static_cast<uint8_t*>(buffer);
    size_t received = 0;
    while (received < size) {
        const int n = recv(socket, reinterpret_cast<char*>(out + received),
                           static_cast<int>(size - received), 0);
        if (n <= 0) return false;
        received += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

StreamServer::~StreamServer() {
    Stop();
}

bool StreamServer::Start(uint16_t controlPort, Callbacks callbacks, std::string* error) {
    if (!EnsureWinsock(error)) return false;
    callbacks_ = std::move(callbacks);

    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        if (error) *error = "failed to create the control socket";
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(controlPort);

    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        if (error) *error = "port " + std::to_string(controlPort) + " is already in use";
        closesocket(listener);
        return false;
    }
    if (listen(listener, 1) == SOCKET_ERROR) {
        if (error) *error = "listen failed";
        closesocket(listener);
        return false;
    }

    const SOCKET video = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (video == INVALID_SOCKET) {
        if (error) *error = "failed to create the video socket";
        closesocket(listener);
        return false;
    }
    // A full send buffer should never block the encoder thread.
    int sendBuffer = 4 * 1024 * 1024;
    setsockopt(video, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBuffer),
               sizeof(sendBuffer));

    listenSocket_ = static_cast<uintptr_t>(listener);
    videoSocket_ = static_cast<uintptr_t>(video);
    videoDestination_.resize(sizeof(sockaddr_in));

    running_ = true;
    acceptThread_ = std::thread(&StreamServer::AcceptLoop, this);
    return true;
}

void StreamServer::Stop() {
    if (!running_.exchange(false)) return;

    if (listenSocket_ != kInvalid) {
        closesocket(static_cast<SOCKET>(listenSocket_));
        listenSocket_ = kInvalid;
    }
    CloseClient();
    if (acceptThread_.joinable()) acceptThread_.join();

    std::lock_guard lock(videoMutex_);
    if (videoSocket_ != kInvalid) {
        closesocket(static_cast<SOCKET>(videoSocket_));
        videoSocket_ = kInvalid;
    }
}

void StreamServer::CloseClient() {
    std::lock_guard lock(sendMutex_);
    if (clientSocket_ != kInvalid) {
        shutdown(static_cast<SOCKET>(clientSocket_), SD_BOTH);
        closesocket(static_cast<SOCKET>(clientSocket_));
        clientSocket_ = kInvalid;
    }
    clientConnected_ = false;
}

void StreamServer::AcceptLoop() {
    while (running_) {
        sockaddr_in peer{};
        int peerSize = sizeof(peer);
        const SOCKET client =
            accept(static_cast<SOCKET>(listenSocket_), reinterpret_cast<sockaddr*>(&peer), &peerSize);
        if (client == INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }

        // Input latency matters far more than packing efficiency here.
        BOOL noDelay = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
                   sizeof(noDelay));

        char address[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address));
        wprintf(L"client connected from %hs\n", address);

        {
            std::lock_guard lock(videoMutex_);
            // Video goes back to wherever the control connection came from; the
            // port is filled in when START arrives.
            auto* destination = reinterpret_cast<sockaddr_in*>(videoDestination_.data());
            *destination = sockaddr_in{};
            destination->sin_family = AF_INET;
            destination->sin_addr = peer.sin_addr;
        }
        {
            std::lock_guard lock(sendMutex_);
            clientSocket_ = static_cast<uintptr_t>(client);
        }
        clientConnected_ = true;

        ClientLoop(static_cast<uintptr_t>(client));

        if (streaming_) {
            streaming_ = false;
            if (callbacks_.stopSession) callbacks_.stopSession();
        }
        CloseClient();
        wprintf(L"client disconnected\n");
    }
}

void StreamServer::ClientLoop(uintptr_t clientSocket) {
    const SOCKET socket = static_cast<SOCKET>(clientSocket);
    std::vector<uint8_t> payload;

    while (running_) {
        uint32_t length = 0;
        if (!RecvExact(socket, &length, sizeof(length))) break;
        if (length == 0 || length > 1 * 1024 * 1024) break;  // absurd frame: drop the client

        payload.resize(length);
        if (!RecvExact(socket, payload.data(), length)) break;

        const auto type = static_cast<protocol::MessageType>(payload[0]);
        HandleMessage(type, payload.data() + 1, length - 1);
    }
}

void StreamServer::HandleMessage(protocol::MessageType type, const uint8_t* data, size_t size) {
    protocol::Reader reader(data, size);

    switch (type) {
        case protocol::MessageType::Hello: {
            uint16_t version = 0;
            std::string name;
            reader.U16(version);
            reader.Str(name);
            wprintf(L"hello from \"%hs\" (protocol v%u)\n", name.c_str(), version);
            // The game library is what the client actually shows. The window
            // list still follows it, both as a fallback and for streaming
            // something that is already running.
            SendGameList();
            SendWindowList();
            break;
        }
        case protocol::MessageType::Start:
            HandleStart(reader);
            break;

        case protocol::MessageType::Launch:
            HandleLaunch(reader);
            break;

        case protocol::MessageType::CoverRequest: {
            std::string gameId;
            if (reader.Str(gameId)) SendCover(gameId);
            break;
        }

        case protocol::MessageType::Stop:
            if (streaming_.exchange(false) && callbacks_.stopSession) callbacks_.stopSession();
            break;

        case protocol::MessageType::RequestIdr:
            if (callbacks_.onRequestIdr) callbacks_.onRequestIdr();
            break;

        case protocol::MessageType::Gamepad: {
            protocol::GamepadState state{};
            if (!reader.Raw(&state, sizeof(state))) {
                wprintf(L"malformed GAMEPAD message (%zu bytes, expected %zu)\n", size,
                        sizeof(state));
                break;
            }
            if (!callbacks_.onGamepad) {
                if (!warnedNoGamepadSink_) {
                    warnedNoGamepadSink_ = true;
                    wprintf(L"input received but no virtual pad is attached - ignoring\n");
                }
                break;
            }
            if (!sawGamepadInput_) {
                sawGamepadInput_ = true;
                wprintf(L"receiving gamepad input from the client (first packet: ");
                for (size_t i = 0; i < size && i < sizeof(state); ++i) wprintf(L"%02X ", data[i]);
                wprintf(L") buttons=0x%04X lx=%d\n", state.buttons, state.leftStickX);
            }
            callbacks_.onGamepad(state);
            break;
        }
        case protocol::MessageType::MouseMove: {
            uint16_t x = 0, y = 0;
            if (reader.U16(x) && reader.U16(y)) InputInjector::MoveMouse(x, y);
            break;
        }

        case protocol::MessageType::MouseButton: {
            uint8_t button = 0, pressed = 0;
            if (reader.U8(button) && reader.U8(pressed)) {
                InputInjector::MouseButtonEvent(
                    static_cast<InputInjector::MouseButton>(button), pressed != 0);
            }
            break;
        }

        case protocol::MessageType::MouseScroll: {
            uint16_t delta = 0;
            if (reader.U16(delta)) {
                InputInjector::Scroll(static_cast<int16_t>(delta));
            }
            break;
        }

        case protocol::MessageType::Key: {
            uint16_t key = 0;
            uint8_t pressed = 0;
            if (reader.U16(key) && reader.U8(pressed)) {
                InputInjector::KeyEvent(key, pressed != 0);
            }
            break;
        }

        case protocol::MessageType::Text: {
            std::string utf8;
            if (!reader.Str(utf8) || utf8.empty()) break;
            const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                                 static_cast<int>(utf8.size()), nullptr, 0);
            std::wstring wide(static_cast<size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                wide.data(), size);
            InputInjector::TypeText(wide);
            break;
        }

        case protocol::MessageType::Ping: {
            uint64_t clientTime = 0;
            if (reader.U64(clientTime)) {
                protocol::Writer out;
                out.U64(clientTime);
                SendMessage(protocol::MessageType::Pong, out);
            }
            break;
        }
        default:
            break;
    }
}

void StreamServer::SendWindowList() {
    if (!callbacks_.listWindows) return;
    const auto windows = callbacks_.listWindows();

    protocol::Writer out;
    out.U16(static_cast<uint16_t>(windows.size()));
    for (const auto& window : windows) {
        out.U64(reinterpret_cast<uint64_t>(window.hwnd));
        out.U32(static_cast<uint32_t>(window.clientWidth));
        out.U32(static_cast<uint32_t>(window.clientHeight));
        out.Str(Utf8(window.process));
        out.Str(Utf8(window.title));
    }
    SendMessage(protocol::MessageType::WindowList, out);
}

void StreamServer::SendGameList() {
    if (!callbacks_.listGames) return;
    const auto games = callbacks_.listGames();

    protocol::Writer out;
    out.U16(static_cast<uint16_t>(games.size()));
    for (const auto& game : games) {
        out.Str(game.id);
        out.Str(game.name);
        out.Str(game.platform);
        out.Str(game.source);
        out.U8(game.installed ? 1 : 0);
    }
    SendMessage(protocol::MessageType::GameList, out);
    wprintf(L"sent %zu games from the Playnite library\n", games.size());
}

void StreamServer::SendCover(const std::string& gameId) {
    std::vector<uint8_t> jpeg;

    if (callbacks_.listGames) {
        // Cheap enough at this size, and it keeps the server from holding its own
        // copy of the library that could go stale behind Playnite's back.
        for (const auto& game : callbacks_.listGames()) {
            if (game.id != gameId) continue;
            if (!game.coverPath.empty()) {
                CoverArt::LoadThumbnail(game.coverPath, protocol::kCoverWidth, &jpeg);
            }
            break;
        }
    }

    // Always reply, even with nothing: a client waiting on a cover that will
    // never arrive would leave a permanently blank tile.
    protocol::Writer out;
    out.Str(gameId);
    out.U32(static_cast<uint32_t>(jpeg.size()));
    out.Raw(jpeg.data(), jpeg.size());
    SendMessage(protocol::MessageType::CoverData, out);
}

void StreamServer::SendLaunchProgress(const std::string& message) {
    protocol::Writer out;
    out.Str(message);
    SendMessage(protocol::MessageType::LaunchProgress, out);
}

void StreamServer::HandleLaunch(protocol::Reader& reader) {
    LaunchRequest request;
    uint32_t width = 0, height = 0, fps = 0, bitrate = 0;
    uint8_t codec = 0;
    uint16_t udpPort = 0;

    if (!reader.Str(request.gameId) || !reader.U32(width) || !reader.U32(height) ||
        !reader.U32(fps) || !reader.U32(bitrate) || !reader.U8(codec) || !reader.U16(udpPort)) {
        SendError("malformed LAUNCH");
        return;
    }

    request.width = static_cast<int>(width);
    request.height = static_cast<int>(height);
    request.fps = fps > 0 ? static_cast<int>(fps) : 60;
    request.bitrateKbps = bitrate > 0 ? static_cast<int>(bitrate) : 20000;
    request.codec = static_cast<protocol::Codec>(codec);
    request.clientUdpPort = udpPort != 0 ? udpPort : protocol::kDefaultVideoPort;

    {
        std::lock_guard lock(videoMutex_);
        auto* destination = reinterpret_cast<sockaddr_in*>(videoDestination_.data());
        destination->sin_port = htons(request.clientUdpPort);
    }

    int actualWidth = 0, actualHeight = 0;
    std::vector<uint8_t> sequenceHeader;
    std::string error;

    if (!callbacks_.launchGame ||
        !callbacks_.launchGame(request, &actualWidth, &actualHeight, &sequenceHeader, &error)) {
        SendError(error.empty() ? "could not launch that game" : error);
        return;
    }

    frameNumber_ = 0;
    streaming_ = true;

    protocol::Writer out;
    out.U32(static_cast<uint32_t>(actualWidth));
    out.U32(static_cast<uint32_t>(actualHeight));
    out.U8(static_cast<uint8_t>(request.codec));
    out.U16(static_cast<uint16_t>(sequenceHeader.size()));
    out.Raw(sequenceHeader.data(), sequenceHeader.size());
    SendMessage(protocol::MessageType::Started, out);

    if (callbacks_.onRequestIdr) callbacks_.onRequestIdr();
    wprintf(L"streaming %dx%d to udp port %u\n", actualWidth, actualHeight, request.clientUdpPort);
}

void StreamServer::HandleStart(protocol::Reader& reader) {
    StartRequest request;
    uint32_t width = 0, height = 0, fps = 0, bitrate = 0;
    uint8_t codec = 0;
    uint16_t udpPort = 0;

    if (!reader.U64(request.windowId) || !reader.U32(width) || !reader.U32(height) ||
        !reader.U32(fps) || !reader.U32(bitrate) || !reader.U8(codec) || !reader.U16(udpPort)) {
        SendError("malformed START");
        return;
    }

    request.width = static_cast<int>(width);
    request.height = static_cast<int>(height);
    request.fps = fps > 0 ? static_cast<int>(fps) : 60;
    request.bitrateKbps = bitrate > 0 ? static_cast<int>(bitrate) : 20000;
    request.codec = static_cast<protocol::Codec>(codec);
    request.clientUdpPort = udpPort != 0 ? udpPort : protocol::kDefaultVideoPort;

    {
        std::lock_guard lock(videoMutex_);
        auto* destination = reinterpret_cast<sockaddr_in*>(videoDestination_.data());
        destination->sin_port = htons(request.clientUdpPort);
    }

    int actualWidth = 0, actualHeight = 0;
    std::vector<uint8_t> sequenceHeader;
    std::string error;

    if (!callbacks_.startSession ||
        !callbacks_.startSession(request, &actualWidth, &actualHeight, &sequenceHeader, &error)) {
        SendError(error.empty() ? "failed to start the capture session" : error);
        return;
    }

    frameNumber_ = 0;
    streaming_ = true;

    protocol::Writer out;
    out.U32(static_cast<uint32_t>(actualWidth));
    out.U32(static_cast<uint32_t>(actualHeight));
    out.U8(static_cast<uint8_t>(request.codec));
    out.U16(static_cast<uint16_t>(sequenceHeader.size()));
    out.Raw(sequenceHeader.data(), sequenceHeader.size());
    SendMessage(protocol::MessageType::Started, out);

    // The encoder's opening IDR was produced before `streaming_` went true, so it
    // was dropped rather than sent. Force another: without one the client decodes
    // P-frames against a reference it never received and shows nothing at all.
    if (callbacks_.onRequestIdr) callbacks_.onRequestIdr();

    wprintf(L"streaming %dx%d to udp port %u\n", actualWidth, actualHeight, request.clientUdpPort);
}

void StreamServer::SendMessage(protocol::MessageType type, const protocol::Writer& payload) {
    std::lock_guard lock(sendMutex_);
    if (clientSocket_ == kInvalid) return;

    const uint32_t length = static_cast<uint32_t>(payload.Size() + 1);
    std::vector<uint8_t> frame;
    frame.reserve(sizeof(length) + length);
    frame.insert(frame.end(), reinterpret_cast<const uint8_t*>(&length),
                 reinterpret_cast<const uint8_t*>(&length) + sizeof(length));
    frame.push_back(static_cast<uint8_t>(type));
    frame.insert(frame.end(), payload.Bytes().begin(), payload.Bytes().end());

    const SOCKET socket = static_cast<SOCKET>(clientSocket_);
    size_t sent = 0;
    while (sent < frame.size()) {
        const int n = send(socket, reinterpret_cast<const char*>(frame.data() + sent),
                           static_cast<int>(frame.size() - sent), 0);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

void StreamServer::SendError(const std::string& message) {
    protocol::Writer out;
    out.Str(message);
    SendMessage(protocol::MessageType::Error, out);
}

void StreamServer::SendVideoFrame(const uint8_t* data, size_t size, uint64_t timestampMicros,
                                  bool keyframe) {
    if (!streaming_ || size == 0) return;

    std::lock_guard lock(videoMutex_);
    if (videoSocket_ == kInvalid) return;

    const auto* destination = reinterpret_cast<const sockaddr_in*>(videoDestination_.data());
    if (destination->sin_port == 0) return;

    const uint32_t frame = frameNumber_.fetch_add(1);
    const size_t fragmentCount =
        (size + protocol::kMaxVideoPayload - 1) / protocol::kMaxVideoPayload;

    uint8_t datagram[protocol::kMaxDatagram];
    for (size_t index = 0; index < fragmentCount; ++index) {
        const size_t offset = index * protocol::kMaxVideoPayload;
        const size_t chunk = (std::min)(protocol::kMaxVideoPayload, size - offset);

        protocol::VideoFragmentHeader header{};
        header.magic = protocol::kVideoMagic;
        header.frameNumber = frame;
        header.fragmentIndex = static_cast<uint16_t>(index);
        header.fragmentCount = static_cast<uint16_t>(fragmentCount);
        header.timestampMicros = timestampMicros;
        header.flags = keyframe ? protocol::kFlagKeyframe : 0;
        header.payloadSize = static_cast<uint16_t>(chunk);

        memcpy(datagram, &header, sizeof(header));
        memcpy(datagram + sizeof(header), data + offset, chunk);

        sendto(static_cast<SOCKET>(videoSocket_), reinterpret_cast<const char*>(datagram),
               static_cast<int>(sizeof(header) + chunk), 0,
               reinterpret_cast<const sockaddr*>(destination), sizeof(sockaddr_in));
    }
}

}  // namespace thorstream
