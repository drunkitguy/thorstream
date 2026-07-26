#pragma once

#include <Windows.h>

#include <string>

namespace thorstream {

// A locked Windows session stops compositing the user's desktop, so
// Windows.Graphics.Capture is given almost no frames - measured here at 3.3 fps
// against 53 fps unlocked. The stream still "works", just terribly, which is a
// miserable thing to debug from a handheld. So a game launched from the couch
// has to unlock the machine first.
//
// It does that the way Sunshine and Apollo do: a Terminal Services reconnect of
// the session to the console. When that call comes from SYSTEM, Windows does not
// re-authenticate, so the session unlocks with no password involved anywhere.
// Nothing here stores, prompts for, or transmits credentials.
//
// The host itself runs as you, not SYSTEM - it has to, because
// Windows.Graphics.Capture cannot see your desktop from session 0. So the
// privileged half lives in a separate on-demand scheduled task that runs this
// same binary with --unlock-now, and the host merely asks it to run.
class SessionLock {
public:
    // True when the secure desktop is in front, i.e. the machine is locked.
    static bool IsLocked();

    // True when the SYSTEM-side helper task is registered. Unlocking is opt-in:
    // installing it is a separate, explicitly elevated step.
    static bool UnlockHelperInstalled();

    // Triggers the helper task and waits for the lock to actually clear, rather
    // than trusting the task's exit code - the reconnect is asynchronous and the
    // secure desktop takes a moment to go away.
    static bool RequestUnlock(int timeoutSeconds, std::string* error);

    // The privileged half. Only succeeds as SYSTEM; run by the helper task.
    static bool UnlockNow();

    // Keeps the session awake for the duration of a stream: no screensaver, no
    // display sleep, and therefore no screensaver-triggered lock. Reversible and
    // per-user; nothing here needs administrator rights.
    static void PreventLocking();
    static void AllowLocking();
};

}  // namespace thorstream
