#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace thorstream {

// Answers "is there a thorstream host here?" broadcasts so a client can find
// this PC without anyone typing an IP address.
//
// A plain UDP broadcast beacon rather than mDNS: no dependency on a Bonjour-style
// responder being installed and working, no service registration to leak if the
// host is killed, and the whole exchange is two datagrams.
class DiscoveryResponder {
public:
    ~DiscoveryResponder();

    bool Start(uint16_t discoveryPort, uint16_t controlPort, std::string* error);
    void Stop();

private:
    void Loop();

    uintptr_t socket_ = ~uintptr_t{0};
    uint16_t controlPort_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

namespace discovery {
inline constexpr uint16_t kDefaultPort = 47809;
// Deliberately distinct strings so a stray packet cannot be mistaken for either.
inline constexpr char kProbe[] = "THORSTREAM-DISCOVER-1";
inline constexpr char kReply[] = "THORSTREAM-HOST-1";
}  // namespace discovery

}  // namespace thorstream
