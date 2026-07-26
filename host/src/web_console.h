#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace thorstream {

// What the status page shows. Filled by main, which is the only place that knows
// about all of the moving parts at once.
struct HostStatus {
    bool serving = false;      // the control listener is accepting clients
    std::string lastError;     // why it is not, if it is not
    int controlPort = 0;
    uint64_t uptimeSeconds = 0;      // of the host process
    uint64_t servingSeconds = 0;     // since the listener last came up
    bool clientConnected = false;
    bool streaming = false;
    uint64_t framesSent = 0;
    uint64_t bytesSent = 0;
    std::string gamepad;
    std::string logPath;
    std::vector<std::string> addresses;
};

// A small control page for the host, because a GUI-subsystem background process
// is otherwise completely opaque: no window, no console, and a log file you
// cannot tell apart from a stale one.
//
// It runs inside the host process and stays up even when streaming is stopped.
// That is deliberate. If Stop killed the process, the page serving the Start
// button would die with it, and the only way back would be the command line -
// which is the problem this is meant to solve. So Stop and Start bring the
// control listener down and up in place, and the process itself keeps running.
class WebConsole {
public:
    ~WebConsole();

    // Supplied by main. onStart and onRestart report failure through *error so
    // the page can show why rather than just refusing to change state.
    std::function<HostStatus()> status;
    std::function<bool(std::string*)> onStart;
    std::function<void()> onStop;
    std::function<bool(std::string*)> onRestart;

    bool Start(uint16_t port, std::string* error);
    void Stop();

private:
    void AcceptLoop();
    void HandleConnection(uintptr_t socket);

    uintptr_t listenSocket_ = ~uintptr_t{0};
    std::thread thread_;
    std::atomic<bool> running_{false};
};

namespace web {
// One past the video port, so the whole host occupies one tidy range.
inline constexpr uint16_t kDefaultPort = 47812;
}  // namespace web

}  // namespace thorstream
