# thorstream

Stream **one game window** from a Windows PC to an Ayn Thor (or any Android
device), without putting your desktop on screen.

Every other PC-to-handheld streaming setup captures a whole display. This one
captures a single window at the compositor level, so nothing else you have open
— other apps, notifications, a second monitor — can ever appear in the stream.
Not cropped out. Never in the frame buffer to begin with.

|  |  |
|---|---|
| **Host** | C++20, Windows 11, `Windows.Graphics.Capture` + NVENC |
| **Client** | Kotlin, Android 8.0+, `MediaCodec` hardware decode |
| **Input** | Thor's controller → virtual Xbox 360 pad on the PC (ViGEm) |
| **Transport** | TCP control + UDP video on your LAN |

## Status

Working and measured against a real game (007 First Light, 4K):

- 3840×2160 captured and H.264-encoded at **53 fps, ~13 Mbps**
- 9,033 UDP datagrams reassembled by a test client with **0 frames dropped**,
  decoding with zero errors
- Gamepad state verified end to end by reading it back through **XInput**, the
  same API a game uses — 8/8 cases exact

Not yet verified on real hardware: the Android client has been built into an APK
but not yet run on a Thor. See [Testing](#testing).

## Why window capture actually works

`Windows.Graphics.Capture` can build a capture item from an `HWND`
(`IGraphicsCaptureItemInterop::CreateForWindow`). The DWM hands over that
window's redirection surface directly.

The proof is in [FEASIBILITY.md](FEASIBILITY.md): with a **topmost magenta window
covering the entire screen**, a normal screenshot is solid magenta while the
captured frame is clean game content, taken at the same instant.

This is the same API OBS uses for its "Window Capture (WGC)" source, so it is
well-trodden rather than a clever hack.

### Constraints worth knowing

- Capture rate is bounded by the compositor, i.e. your monitor's refresh rate.
- Frames arrive **only when the window changes**. The host re-sends the last
  frame after 200 ms of stillness so the client's decoder never starves.
- The capture surface includes window chrome. The host crops to the client area
  using DWM's *extended frame bounds* — not `GetWindowRect`, which includes an
  invisible resize border and silently costs ~10 columns of real pixels.
- Minimised windows produce no frames.
- Games in **legacy exclusive fullscreen** are untested. Borderless fullscreen —
  what modern games default to — is confirmed working. If a game misbehaves,
  switch it to borderless in its own video settings.

## Building

### Host (Windows)

Needs Visual Studio with the **Desktop development with C++** workload (MSVC +
Windows 11 SDK), CMake, and Ninja. An NVIDIA GPU is required for NVENC; the
NVENC headers are vendored, and the runtime ships with your display driver.

```bash
host\build.bat
```

For gamepad input, install [ViGEmBus](https://github.com/nefarius/ViGEmBus):

```bash
winget install ViGEm.ViGEmBus
```

### Client (Android)

Needs JDK 17 and the Android SDK (API 35). Point `client/local.properties` at
your SDK, then:

```bash
cd client && gradlew.bat assembleDebug
```

The APK lands in `client/app/build/outputs/apk/debug/`.

## Running

Grab the [latest release](https://github.com/drunkitguy/thorstream/releases/latest)
if you would rather not build anything.

**On the PC**, start the server:

```bash
host\build\thorstream-host.exe --serve
```

It prints the addresses to connect to.

**On the Thor**, download the APK from the release page in the device's browser
and open it. Android will ask you to allow installing from your browser — that
is expected for a sideloaded app, and only needs granting once. The APK is
debug-signed, which is what lets it install without a Play Store account.

Then open the app, type the IP the host printed, tap Connect, and pick a window
from the list.

The host also has a standalone capture mode, useful for checking a specific game
before involving the network at all:

```bash
host\build\thorstream-host.exe --encode --seconds 10
```

With no window filter it prints a numbered list and prompts. It reports the
achieved framerate and writes an `.h264` file you can play in VLC.

## Running it automatically

To have the host start with Windows and stay up:

```powershell
host\autostart.ps1 -Install
```

That registers a Scheduled Task which starts the host at logon, restarts it if
it stops unexpectedly, runs with no visible console window, and logs to
`%LOCALAPPDATA%\thorstream\host.log`.

```powershell
host\autostart.ps1 -Status      # is it installed, running, and listening?
host\autostart.ps1 -Uninstall
```

`-Status` reports the only thing that really matters — whether the port is
accepting connections — along with the addresses to type into the client.

Pass `-ShowWindow` if you would rather see the console, or `-Port` to use a
different port. No administrator rights are needed.

### Why not a Windows service?

Services run in session 0, which is isolated from your desktop.
`Windows.Graphics.Capture` can only see windows in the interactive session, so a
service would start perfectly happily and capture nothing at all. A logon task
runs in your session, which is where the windows are.

## Popups

Dialogs that appear over the game are separate windows, so capturing only the
game means they never reach the stream — you get a game that has silently
stopped responding because something is asking a question you cannot see.

Qualifying popups are captured as their own windows and composited over the game
frame. That keeps the premise intact: we still only ever capture windows we have
explicitly chosen, rather than falling back to capturing the desktop.

**This does loosen the isolation, and it is worth being clear about.** A
notification containing something private can now reach the stream, where
previously nothing outside the game window could. Turn it off with
`--no-popups` if that matters more than seeing dialogs.

What counts as a popup is deliberately narrow, because the first version was
not: it accepted any window with popup-ish style bits and promptly composited
the Settings app over the entire frame, hiding the game completely. A popup must
now be **smaller than 85% of the game window** — a dialog interrupts something,
it does not replace it — must not be a shell surface or app frame, and must be a
dialog, owned, topmost or popup-styled.

Popups are re-composited even while the game is idle. A modal dialog usually
stops the game rendering, which is exactly when the popup most needs drawing, so
the host keeps a copy of the last game frame to draw onto.

## Colour

The bitstream declares its colour space explicitly: **BT.709, studio range**,
written into the VUI. That matters more than it sounds. Without it a decoder has
to guess, and a wrong guess is subtle rather than obvious — the picture still
appears, it is just the wrong colour. Decoding studio-range data as full range
washes out blacks and clips highlights; decoding BT.709 as BT.601 shifts hues,
worst in reds and greens.

Measured round trip through capture, encode and decode, with no overrides:

| Source | Decoded |
|---|---|
| white `255,255,255` | `255,255,255` |
| black `0,0,0` | `0,0,0` |
| skin `224,172,139` | `223,171,137` |

For slightly better fidelity, run the host with `--full-range` (or
`autostart.ps1 -Install -FullRange`). That keeps all 256 levels per channel
instead of compressing into 16–235, which measurably improves greys and reduces
banding in gradients. It is not the default because it depends on the client
honouring the range flag; one that ignores it renders the picture too contrasty.
Try it, and if anything looks crushed, drop back.

Chroma is 4:2:0, which is inherent to ordinary H.264. Fine for game content;
it is what causes slight colour fringing on thin coloured text.

## Checking it works

The host prints its state on startup. A working server looks like this:

```
=== thorstream host ===
Virtual Xbox 360 pad ready.
Listening for a client on port 47810.
  Point the Thor client at one of these:
    192.168.0.3:47810
```

If you want to confirm independently that it is listening:

```bash
netstat -ano | findstr 47810
```

You should see `0.0.0.0:47810  LISTENING`. If that line is missing, the server
did not start — check the console for an error.

### Firewall

Windows will prompt the first time the host listens. Allow it on **both**
private and public networks: home Wi-Fi is often classified as Public, and a
private-only rule will leave the handheld silently unable to connect.

The rule is tied to the executable's path, so Windows will prompt again if you
move the `.exe` or swap a locally built binary for a downloaded release.

To check what is already allowed:

```powershell
Get-NetFirewallRule -DisplayName "*thorstream*" | Select-Object DisplayName, Profile, Enabled
```

### The controller does nothing

On startup the host reports which XInput slot the driver gave it:

```
Virtual Xbox 360 pad ready (XInput slot 0).
```

If it says the pad has **no** slot, or games ignore it while the host logs
`receiving gamepad input from the client`, the usual cause is an **orphaned
virtual pad** squatting on the slot. ViGEm creates a device node per pad, and a
host that is killed rather than stopped can leave one behind; the orphan takes
the XInput slot and never responds, so a new pad is shadowed by a dead one.

To check, stop every host and list what remains:

```powershell
Get-PnpDevice -Class XnaComposite | Select-Object Status, InstanceId
```

With no host running there should be **nothing** with status `OK`. Anything
still `OK` and located on the "Virtual Gamepad Emulation Bus" is an orphan. A
reboot clears them.

This should not happen any more — the host installs a console control handler
and unplugs the pad on Ctrl+C — but a hard kill (`taskkill /F`, a crash) can
still strand one.

To rule out the network entirely and test the driver on its own:

```bash
thorstream-host.exe --gamepad-selftest
```

### The client cannot see the host

- Confirm both devices are on the same network — a "guest" Wi-Fi network is
  usually isolated from the wired LAN by design.
- Confirm the IP. The host prints every address it can find; use the one on the
  same subnet as the handheld.
- Some routers block client-to-client traffic ("AP isolation"); this must be off.

## Testing

`tools/ProtocolTestClient` is a reference client that exercises the whole
protocol from a PC, so protocol bugs surface in C# rather than inside an Android
app where they are far harder to see.

```bash
# stream a window and verify the received bitstream decodes
ProtocolTestClient.exe 127.0.0.1 47810 "007" 8

# drive the virtual gamepad and read it back through XInput
ProtocolTestClient.exe --gamepad
```

## Security

**There is no encryption and no authentication.** This is a LAN protocol between
two machines you own.

Do not port-forward 47810 or 47811 to the internet. Anyone who can reach the
control port can enumerate your open windows, stream any of them, and inject
gamepad input.

## Layout

```
host/          C++ host: capture, encode, serve, inject input
  src/         window_capture, encoder_nvenc, net_server, session, gamepad_vigem
  third_party/ ffnvcodec (NVENC headers), vigem (ViGEmClient)
client/        Kotlin Android client
tools/         C# reference client for protocol and gamepad testing
poc/           the original feasibility probes
```

Protocol is documented in [PROTOCOL.md](PROTOCOL.md).

## Licences

Vendored third-party code, all permissive:

- [nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) — NVENC API
  header, MIT-style (notice at the top of the header)
- [ViGEmClient](https://github.com/nefarius/ViGEmClient) — MIT
