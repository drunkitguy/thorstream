#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

namespace thorstream {

struct GameEntry {
    std::string id;  // Playnite GUID
    std::string name;
    std::string platform;
    std::string source;
    std::string installDirectory;
    // Absolute path to Playnite's cover art, or empty.
    std::wstring coverPath;
    bool installed = false;
};

// The user's Playnite library, and the ability to start something from it.
//
// Playnite's database is LiteDB v4, which needs a matching LiteDB to read.
// Rather than reimplement that format, the host shells out to a small
// self-contained helper - which also keeps that library's known CVE out of this
// process, since it only ever parses the user's own local file.
class GameLibrary {
public:
    // Empty if Playnite is not installed or the helper is missing.
    static std::vector<GameEntry> Read(std::string* error);

    // Starts a game through Playnite itself, so Steam, Epic, EA, Xbox and
    // emulator entries all launch the same way they would from its own UI.
    static bool Launch(const std::string& gameId, std::string* error);

    // Waits for a window belonging to a process that started after `since`.
    // Games take a long time to show anything, hence the generous timeout.
    //
    // The wait blocks the thread that started the launch, so it takes an
    // optional predicate that another thread can make true to abandon it: an
    // already-running game never starts a new process, and waiting the full
    // timeout for that would make the host deaf to a stop or a shutdown for
    // minutes. Polled once per second-ish, so it must be cheap and thread-safe.
    static HWND WaitForNewGameWindow(FILETIME since, int timeoutSeconds,
                                     const std::vector<HWND>& ignore,
                                     const std::function<bool()>& abandon = {});
};

}  // namespace thorstream
