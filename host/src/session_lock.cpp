#include "session_lock.h"

#include <wtsapi32.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace thorstream {
namespace {

// Registered by autostart.ps1 -InstallUnlock. Deliberately a separate task from
// the host's own autostart entry: this one runs as SYSTEM, and conflating the
// two would silently give the whole host privileges it does not need.
constexpr wchar_t kUnlockTaskName[] = L"thorstream-unlock";

// Runs a command with no window and waits for it. Returns the exit code, or -1
// if the process could not be started at all.
int RunHidden(std::wstring commandLine, int timeoutMs) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION process{};
    // CreateProcessW may write to the command line buffer, hence the by-value
    // std::wstring rather than a literal.
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        return -1;
    }

    WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeoutMs));
    DWORD exitCode = static_cast<DWORD>(-1);
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}

std::wstring SystemPath(const wchar_t* exe) {
    wchar_t directory[MAX_PATH] = {};
    if (!GetSystemDirectoryW(directory, MAX_PATH)) return exe;
    return std::wstring(directory) + L"\\" + exe;
}

// Whether we are running as SYSTEM. Only used to make a failed unlock explain
// itself: "did nothing" and "was not allowed to" look identical otherwise.
bool RunningAsSystem() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::string buffer(needed, '\0');
    auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());

    bool isSystem = false;
    if (needed && GetTokenInformation(token, TokenUser, user, needed, &needed)) {
        PSID systemSid = nullptr;
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
        if (AllocateAndInitializeSid(&authority, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0,
                                     &systemSid)) {
            isSystem = EqualSid(user->User.Sid, systemSid) != FALSE;
            FreeSid(systemSid);
        }
    }
    CloseHandle(token);
    return isSystem;
}

}  // namespace

bool SessionLock::IsLocked() {
    // When the machine is locked, Winlogon's secure desktop is the input
    // desktop. A normal user-session process cannot open it, and even when it
    // can, the name is "Winlogon" rather than "Default". Checking the desktop
    // beats looking for the LogonUI process, which lingers in some states.
    const HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) return true;  // access denied means the secure desktop is up

    wchar_t name[128] = {};
    DWORD needed = 0;
    const bool named =
        GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed) != FALSE;
    CloseDesktop(desktop);

    if (!named) return true;
    return _wcsicmp(name, L"Default") != 0;
}

bool SessionLock::UnlockHelperInstalled() {
    const std::wstring command =
        L"\"" + SystemPath(L"schtasks.exe") + L"\" /Query /TN \"" + kUnlockTaskName + L"\"";
    return RunHidden(command, 10000) == 0;
}

bool SessionLock::RequestUnlock(int timeoutSeconds, std::string* error) {
    if (!IsLocked()) return true;

    if (!UnlockHelperInstalled()) {
        *error =
            "the PC is locked and the unlock helper is not installed - run "
            "autostart.ps1 -InstallUnlock from an elevated PowerShell";
        return false;
    }

    wprintf(L"Session is locked; asking the unlock helper to reconnect it...\n");
    const std::wstring command =
        L"\"" + SystemPath(L"schtasks.exe") + L"\" /Run /TN \"" + kUnlockTaskName + L"\"";
    const int rc = RunHidden(command, 15000);
    if (rc != 0) {
        *error = "could not start the unlock helper task (schtasks exit " + std::to_string(rc) + ")";
        return false;
    }

    // schtasks returns as soon as the task is queued, and the reconnect itself
    // is asynchronous, so the only trustworthy signal is the lock actually
    // clearing. Poll rather than sleeping a fixed amount.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!IsLocked()) {
            wprintf(L"Session unlocked.\n");
            // The desktop switch races the compositor: capturing immediately
            // after gets a few frames of the old, starved rate.
            std::this_thread::sleep_for(std::chrono::milliseconds(750));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    *error = "the unlock helper ran but the session was still locked after " +
             std::to_string(timeoutSeconds) + " seconds";
    return false;
}

bool SessionLock::UnlockNow() {
    const DWORD console = WTSGetActiveConsoleSessionId();
    wprintf(L"Unlock helper: console session %lu, running as %s\n", console,
            RunningAsSystem() ? L"SYSTEM" : L"NOT SYSTEM (this will fail)");

    if (console == 0xFFFFFFFF) {
        wprintf(L"No console session is attached; nothing to unlock.\n");
        return false;
    }

    // Reconnecting the console session to itself is what tscon does, and what
    // clears the lock. The empty password is not a bypass of authentication:
    // WTSConnectSession only skips the password check for a SYSTEM caller, which
    // is precisely why this half runs from a scheduled task.
    if (WTSConnectSessionW(console, console, const_cast<wchar_t*>(L""), TRUE)) {
        wprintf(L"WTSConnectSession succeeded.\n");
        return true;
    }
    const DWORD wtsError = GetLastError();
    wprintf(L"WTSConnectSession failed (error %lu); falling back to tscon.\n", wtsError);

    // WTSConnectSession is documented as a Remote Desktop Session Host API and
    // does refuse on some client SKUs. tscon.exe reaches the same place through
    // winsta.dll and is present on every Windows 11 install.
    const std::wstring command = L"\"" + SystemPath(L"tscon.exe") + L"\" " +
                                 std::to_wstring(console) + L" /dest:console";
    const int rc = RunHidden(command, 15000);
    wprintf(L"tscon exit %d\n", rc);
    return rc == 0;
}

void SessionLock::PreventLocking() {
    // Stops the display sleeping and the screensaver starting, which between
    // them cover every automatic path to a locked session. It does nothing about
    // someone locking the machine deliberately - that is what the helper is for.
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
}

void SessionLock::AllowLocking() {
    SetThreadExecutionState(ES_CONTINUOUS);
}

}  // namespace thorstream
