#include "discovery.h"

#include <WinSock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

namespace thorstream {
namespace {

constexpr uintptr_t kInvalid = ~uintptr_t{0};

std::string ComputerName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(name, &size)) return "PC";

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(size), nullptr, 0,
                                          nullptr, nullptr);
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(size), result.data(), bytes, nullptr,
                        nullptr);
    return result;
}

}  // namespace

DiscoveryResponder::~DiscoveryResponder() {
    Stop();
}

bool DiscoveryResponder::Start(uint16_t discoveryPort, uint16_t controlPort, std::string* error) {
    controlPort_ = controlPort;

    const SOCKET handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == INVALID_SOCKET) {
        if (error) *error = "could not create the discovery socket";
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
    BOOL broadcast = TRUE;
    setsockopt(handle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast),
               sizeof(broadcast));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(discoveryPort);

    if (bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        if (error) {
            *error = "discovery port " + std::to_string(discoveryPort) + " is already in use";
        }
        closesocket(handle);
        return false;
    }

    // A timeout keeps the loop responsive to Stop() without a second signal.
    DWORD timeout = 500;
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));

    socket_ = static_cast<uintptr_t>(handle);
    running_ = true;
    thread_ = std::thread(&DiscoveryResponder::Loop, this);
    return true;
}

void DiscoveryResponder::Stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (socket_ != kInvalid) {
        closesocket(static_cast<SOCKET>(socket_));
        socket_ = kInvalid;
    }
}

void DiscoveryResponder::Loop() {
    const std::string name = ComputerName();

    // "THORSTREAM-HOST-1|<name>|<control port>"
    const std::string reply =
        std::string(discovery::kReply) + "|" + name + "|" + std::to_string(controlPort_);

    char buffer[512];
    while (running_) {
        sockaddr_in from{};
        int fromSize = sizeof(from);
        const int received = recvfrom(static_cast<SOCKET>(socket_), buffer, sizeof(buffer) - 1, 0,
                                      reinterpret_cast<sockaddr*>(&from), &fromSize);
        if (received <= 0) continue;  // timeout, or the socket is going away

        buffer[received] = '\0';
        if (std::string(buffer).rfind(discovery::kProbe, 0) != 0) continue;

        // Reply straight back to the sender. The client learns our address from
        // the packet's source, so there is no need to enumerate interfaces here
        // and guess which one it can actually reach.
        sendto(static_cast<SOCKET>(socket_), reply.data(), static_cast<int>(reply.size()), 0,
               reinterpret_cast<sockaddr*>(&from), fromSize);
    }
}

}  // namespace thorstream
