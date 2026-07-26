# Feasibility: capturing ONE game window (not the desktop)

**Verdict: yes.** Verified on this machine, not just in theory.

## Test rig

| | |
|---|---|
| OS | Windows 11 25H2, build 26200 |
| GPU | NVIDIA RTX 4070 Laptop + Intel Arc iGPU |
| SDK | .NET 10.0.301 |

Two throwaway projects under `poc/`:

- **`D3DTestWindow`** — a stand-in for a game. Borderless-ish window, real
  **flip-model D3D11 swapchain** (`FlipDiscard`, `B8G8R8A8_UNorm`), presenting
  uncapped. Renders a cycling background plus a moving four-quadrant marker
  sprite so any captured frame is unmistakably *this window*.
- **`WindowCaptureProbe`** — enumerates real top-level windows, builds a
  `GraphicsCaptureItem` **for a single HWND**, runs a free-threaded frame pool,
  reports framerate, and dumps PNGs of what actually landed in the buffer.

## The mechanism

`Windows.Graphics.Capture` (WGC). The critical call is `CreateForWindow(HWND)`
on `IGraphicsCaptureItemInterop` — the DWM hands us that window's redirection
surface directly. There is no cropping of a desktop capture involved; the
desktop is never in the buffer to begin with.

This is the same API OBS uses for its "Window Capture (WGC)" source, so it is
well-trodden and not fighting the OS.

## Results

```
WGC supported       : True
Borderless capture  : True      <- can suppress the yellow capture outline
Cursor toggle       : True      <- can exclude the mouse cursor
MinUpdateInterval   : True      <- can cap capture fps at the source
Dirty-region info   : True      <- per-frame changed rects, useful for encoding

Frames delivered    : 530 in 10.01s  (53.0 fps average)
Content size        : 1922x1128
```

### Occlusion test — the important one

A **topmost magenta window covering the whole screen** was raised over the game
window. At that exact moment we saved both a normal desktop screenshot and a WGC
frame:

- `DESKTOP-GROUND-TRUTH.png` — solid magenta, "OCCLUDER" text, game invisible.
- `frame-0240.png` — the game window, fully intact, zero magenta.

The capture is immune to whatever is on top of the window. Anything else on the
user's screen — other apps, notifications, private tabs — cannot leak into the
stream.

## Known constraints (be aware before designing around it)

1. **The frame includes the window chrome.** Item size was `1922x1128` for a
   `1920x1080` client area — that is the border + title bar. Crop to
   `GetClientRect` (in physical pixels; the process must be
   per-monitor-DPI-aware) before encoding.
2. **Capture rate is bounded by the compositor**, i.e. monitor refresh. 53 fps
   here on a 60 Hz path. WGC cannot deliver 144 fps from a 60 Hz desktop.
3. **Frames arrive only on change.** A static window delivered exactly 1 frame
   in 15 s. Great for bandwidth, but the encoder needs its own keepalive/repeat
   logic so the client doesn't stall.
4. **Minimised windows stop producing frames.** Needs handling in the UI.
5. **Exclusive fullscreen** — see the real-game result below. Borderless
   fullscreen (what modern games default to) is confirmed working. Legacy
   exclusive-fullscreen DXGI mode remains untested; if a game uses it, the
   fallback is to switch that game to borderless in its own video settings.
6. **Per-process audio** has a matching API (WASAPI process loopback,
   `AUDIOCLIENT_PROCESS_LOOPBACK`), so audio can be scoped to the game too
   rather than grabbing the whole system mix.

## Confirmed against a real game

Not just the synthetic test window — **007 First Light**, captured and encoded
live by the C++ host:

```
Capture surface   : 3840x2160
Item display name : 007 First Light
NVENC ready       : H.264 3840x2160 @ 40000 kbps
Frames captured   : 534 in 10.08s (53.0 fps average)
Encoded region    : 3840x2160  (surface was 3840x2160)
Bitstream         : 16.73 MB, 13.28 Mbps average
```

`ffprobe` on the result: H.264 High profile, 3840x2160, yuv420p, 1 IDR + 533
P-frames, decodes with no errors, and the decoded frames are the game.

Two things this settles:

- The game runs **borderless fullscreen**, so the crop equals the surface — the
  capture is edge-to-edge game content with no chrome to trim.
- A 4K AAA title sustains ~53 fps through capture and hardware encode at a very
  comfortable 13 Mbps. Bitrate headroom is not the constraint on a LAN.

## Reproducing

```bash
dotnet build poc/D3DTestWindow -c Release && dotnet build poc/WindowCaptureProbe -c Release
```

Run the probe with no arguments to get an interactive window list, or pass a
substring of a window title / process name to go straight at it:

```bash
poc/WindowCaptureProbe/bin/Release/net10.0-windows10.0.26100.0/WindowCaptureProbe.exe "Elden Ring"
```

It captures for 10 s, prints the achieved framerate, and writes PNGs to
`probe-frames/` next to the exe.
