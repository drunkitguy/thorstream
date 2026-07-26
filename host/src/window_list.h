#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace thorstream {

struct WindowEntry {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring className;
    std::wstring process;
    DWORD processId = 0;
    int clientWidth = 0;
    int clientHeight = 0;
};

// Top-level windows a user could plausibly want to stream. Filters out the shell,
// tool windows, cloaked UWP shells and anything too small to be a game.
std::vector<WindowEntry> EnumerateCapturableWindows();

// Case-insensitive substring match against title or process name. Returns nullptr
// if nothing matches.
const WindowEntry* FindWindowByFilter(const std::vector<WindowEntry>& windows,
                                      const std::wstring& filter);

}  // namespace thorstream
