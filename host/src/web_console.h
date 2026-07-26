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

// One row of the Applications grid. Deliberately not GameEntry: the console has
// no business knowing about Playnite, install directories or launch ids.
struct ConsoleGame {
    std::string id;
    std::string name;
    bool hasArt = false;       // artwork exists, from either source
    bool hasOverride = false;  // ...and it is one the user supplied
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

    // Applications. `cover` returns the JPEG thumbnail shown in the grid;
    // `setCover` takes the raw uploaded image file, and `clearCover` drops the
    // override so Playnite's own art applies again.
    std::function<std::vector<ConsoleGame>()> games;
    std::function<bool(const std::string& id, std::vector<uint8_t>* jpeg)> cover;
    std::function<bool(const std::string& id, const std::vector<uint8_t>& image,
                       std::string* error)>
        setCover;
    std::function<bool(const std::string& id)> clearCover;

    // The artwork as a downloadable file: the original image where it can be
    // read, rather than the thumbnail the handheld gets. `filename` is the name
    // suggested to the browser and is sanitised by the implementation.
    std::function<bool(const std::string& id, std::vector<uint8_t>* bytes, std::string* filename,
                       std::string* contentType)>
        artFile;

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
