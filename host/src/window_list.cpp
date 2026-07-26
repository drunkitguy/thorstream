#include "window_list.h"

#include <dwmapi.h>
#include <psapi.h>

#include <algorithm>
#include <cwctype>

namespace thorstream {
namespace {

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring ProcessNameOf(DWORD pid) {
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!handle) return L"?";

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring name = L"?";
    if (QueryFullProcessImageNameW(handle, 0, path, &size)) {
        std::wstring full(path, size);
        const size_t slash = full.find_last_of(L'\\');
        name = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
    }
    CloseHandle(handle);
    return name;
}

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lparam) {
    auto* out = reinterpret_cast<std::vector<WindowEntry>*>(lparam);

    if (hwnd == GetShellWindow()) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return TRUE;

    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (style & WS_DISABLED) return TRUE;

    const LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    // Packaged apps park hidden windows on the desktop; DWM flags them cloaked.
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0)
        return TRUE;

    wchar_t title[512] = {};
    if (GetWindowTextW(hwnd, title, ARRAYSIZE(title)) == 0) return TRUE;

    RECT client{};
    if (!GetClientRect(hwnd, &client)) return TRUE;
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    if (w < 64 || h < 64) return TRUE;

    wchar_t className[256] = {};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    WindowEntry entry;
    entry.hwnd = hwnd;
    entry.title = title;
    entry.className = className;
    entry.processId = pid;
    entry.process = ProcessNameOf(pid);
    entry.clientWidth = w;
    entry.clientHeight = h;
    out->push_back(std::move(entry));
    return TRUE;
}

}  // namespace

std::vector<WindowEntry> EnumerateCapturableWindows() {
    std::vector<WindowEntry> windows;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

const WindowEntry* FindWindowByFilter(const std::vector<WindowEntry>& windows,
                                      const std::wstring& filter) {
    const std::wstring needle = ToLower(filter);
    for (const auto& w : windows) {
        if (ToLower(w.title).find(needle) != std::wstring::npos) return &w;
        if (ToLower(w.process).find(needle) != std::wstring::npos) return &w;
    }
    return nullptr;
}

}  // namespace thorstream
