#include "game_library.h"

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

#include "window_list.h"

namespace thorstream {
namespace {

std::wstring HelperPath() {
    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return {};
    std::wstring path = exePath;
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    return path.substr(0, slash + 1) + L"tools\\PlayniteLibrary.exe";
}

// Runs a command and returns whatever it wrote to stdout.
bool CaptureOutput(const std::wstring& commandLine, std::string* output, std::string* error) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &security, 0)) {
        if (error) *error = "could not create a pipe for the helper";
        return false;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd;

    std::wstring mutableCommand = commandLine;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writeEnd);

    if (!started) {
        CloseHandle(readEnd);
        if (error) *error = "could not start the Playnite helper";
        return false;
    }

    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        output->append(buffer, read);
    }

    CloseHandle(readEnd);
    WaitForSingleObject(process.hProcess, 30000);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return true;
}

// Cover paths in the database are relative to Playnite's library\files folder.
std::wstring PlayniteFilesDir() {
    wchar_t appData[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) return {};
    return std::wstring(appData) + L"\\Playnite\\library\\files\\";
}

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return fields;
}

bool ProcessStartedAfter(DWORD processId, FILETIME since) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return false;

    FILETIME created{}, exited{}, kernel{}, user{};
    const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    CloseHandle(process);
    if (!ok) return false;

    return CompareFileTime(&created, &since) >= 0;
}

}  // namespace

std::vector<GameEntry> GameLibrary::Read(std::string* error) {
    std::vector<GameEntry> games;

    const std::wstring helper = HelperPath();
    if (helper.empty() || GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (error) *error = "the Playnite helper is missing (tools\\PlayniteLibrary.exe)";
        return games;
    }

    std::string output;
    if (!CaptureOutput(L"\"" + helper + L"\" --tsv", &output, error)) return games;

    size_t start = 0;
    while (start < output.size()) {
        size_t end = output.find('\n', start);
        if (end == std::string::npos) end = output.size();

        std::string line = output.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        const auto fields = SplitTabs(line);
        if (fields.size() < 6) continue;

        GameEntry game;
        game.id = fields[0];
        game.name = fields[1];
        game.platform = fields[2];
        game.source = fields[3];
        game.installed = fields[4] == "1";
        game.installDirectory = fields[5];
        if (fields.size() > 6 && !fields[6].empty()) {
            game.coverPath = PlayniteFilesDir() + std::wstring(fields[6].begin(), fields[6].end());
        }
        if (!game.name.empty()) games.push_back(std::move(game));
    }

    return games;
}

bool GameLibrary::Launch(const std::string& gameId, std::string* error) {
    if (gameId.empty()) {
        if (error) *error = "no game id";
        return false;
    }

    // Playnite's own URI scheme. Going through Playnite means Steam, Epic, EA,
    // Xbox and emulator entries all start exactly as they would from its UI,
    // including any per-game scripts and emulator arguments.
    const std::string uri = "playnite://playnite/start/" + gameId;
    std::wstring wide(uri.begin(), uri.end());

    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        if (error) {
            *error = "could not launch through Playnite (code " + std::to_string(result) +
                     "); is Playnite installed?";
        }
        return false;
    }
    return true;
}

HWND GameLibrary::WaitForNewGameWindow(FILETIME since, int timeoutSeconds,
                                       const std::vector<HWND>& ignore) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);

    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& window : EnumerateCapturableWindows()) {
            if (std::find(ignore.begin(), ignore.end(), window.hwnd) != ignore.end()) continue;

            // Playnite's own windows appear too; they are not what was asked for.
            if (window.process.find(L"Playnite") != std::wstring::npos) continue;

            // A game's window is a reasonable size. This also filters out
            // launcher splash screens, which are typically small.
            if (window.clientWidth < 640 || window.clientHeight < 480) continue;

            if (ProcessStartedAfter(window.processId, since)) return window.hwnd;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return nullptr;
}

}  // namespace thorstream
