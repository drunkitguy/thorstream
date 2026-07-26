# thorstream wire protocol v1

Two channels, both IPv4, designed for a trusted LAN:

| Channel | Transport | Default port | Carries |
|---|---|---|---|
| Control | TCP (Nagle off) | 47810 | handshake, window list, session setup, input, keepalive |
| Video | UDP | 47811 | encoded H.264/HEVC frames |

All integers are **little-endian**. Strings are `uint16` byte-length followed by
UTF-8 bytes, never NUL-terminated.

Why split them: input and control must not be stuck behind a retransmit of a
video frame that is already too old to matter, and a dropped video fragment
should never stall the channel carrying the next button press. Video is
inherently disposable — a lost frame is fixed by the next keyframe, not by TCP
retrying it three RTTs later.

## Control channel

Every message is framed as:

```
uint32  payloadLength    // bytes after this field, excluding itself
uint8   type
byte[]  payload          // payloadLength - 1 bytes
```

### Client to host

| Type | Name | Payload |
|---|---|---|
| `0x01` | `HELLO` | `uint16 protocolVersion`, `string clientName` |
| `0x03` | `START` | `uint64 windowId`, `uint32 width`, `uint32 height`, `uint32 fps`, `uint32 bitrateKbps`, `uint8 codec` (0 = H.264, 1 = HEVC), `uint16 clientUdpPort` |
| `0x05` | `STOP` | — |
| `0x06` | `REQUEST_IDR` | — |
| `0x07` | `GAMEPAD` | `GamepadState` (see below) |
| `0x08` | `PING` | `uint64 clientTimeMicros` |

`width`/`height` are an **upper bound**, not an exact size. The client knows its
own panel; only the host knows the window's aspect ratio. The host fits the
window inside that box, preserving aspect, never upscaling, and rounding down to
even numbers (H.264 requires them). `0` on an axis means "unconstrained", so
`0, 0` gives the window's native size.

`STARTED` reports the size actually chosen, which is what the client must
configure its decoder with.

### Host to client

| Type | Name | Payload |
|---|---|---|
| `0x02` | `WINDOW_LIST` | `uint16 count`, then per entry: `uint64 windowId`, `uint32 width`, `uint32 height`, `string process`, `string title` |
| `0x04` | `STARTED` | `uint32 width`, `uint32 height`, `uint8 codec`, `uint16 headerLength`, `byte[] sequenceHeader` |
| `0x09` | `PONG` | `uint64 clientTimeMicros` (echoed verbatim) |
| `0x0A` | `ERROR` | `string message` |

`sequenceHeader` is the SPS/PPS (or VPS/SPS/PPS) in Annex-B form, so the client
can configure its decoder before the first frame lands. The encoder also repeats
these inline on every IDR, so a client that ignores this field still works.

`windowId` is the host's `HWND` widened to 64 bits. It is only valid for the
lifetime of that window — always `START` against an id from a fresh
`WINDOW_LIST`.

### GamepadState

Fixed 16 bytes, no framing overhead. Sent only when something actually changes,
plus a floor of one per 100 ms so the host can detect a dead client.

```
uint16  buttons        // bitfield, see below
uint8   leftTrigger    // 0..255
uint8   rightTrigger   // 0..255
int16   leftStickX     // -32768..32767, +X right
int16   leftStickY     // +Y up
int16   rightStickX
int16   rightStickY
uint32  sequence       // monotonic; host ignores anything older than the newest seen
```

Button bits match the XInput layout, because the host injects into a virtual
Xbox 360 pad and a 1:1 mapping means no translation table to get wrong:

| Bit | Button | Bit | Button |
|---|---|---|---|
| 0 | D-pad up | 8 | Left shoulder |
| 1 | D-pad down | 9 | Right shoulder |
| 2 | D-pad left | 12 | A |
| 3 | D-pad right | 13 | B |
| 4 | Start | 14 | X |
| 5 | Back | 15 | Y |
| 6 | Left stick click | | |
| 7 | Right stick click | | |

## Video channel

The host sends UDP datagrams to the address the TCP connection came from, on the
`clientUdpPort` given in `START`. Each datagram is a 24-byte header plus payload,
capped so the whole datagram stays under a 1280-byte MTU — small enough to
survive any sane LAN path without IP fragmentation, which would turn one lost
packet into a lost frame.

```
uint32  magic          // 'TSV1' = 0x31565354
uint32  frameNumber    // monotonic, starts at 0
uint16  fragmentIndex  // 0-based
uint16  fragmentCount  // total fragments in this frame
uint64  timestampMicros
uint8   flags          // bit 0: this frame is a keyframe
uint8   reserved
uint16  payloadSize
byte[]  payload
```

Reassembly rules for the client:

- Buffer fragments by `frameNumber`. A frame is complete when all
  `fragmentCount` fragments have arrived.
- Once a **newer** frame completes, discard any older incomplete frame — an old
  partial frame is worthless, and holding it only adds latency.
- On discarding an incomplete frame, send `REQUEST_IDR`. Rate-limit this to at
  most one per 100 ms, or a burst of loss will produce a burst of keyframes and
  make congestion worse.
- Never wait for a missing fragment. There is no retransmission by design.

## Session lifecycle

```
client                          host
  |-- TCP connect --------------->|
  |-- HELLO --------------------->|
  |<------------- WINDOW_LIST ----|
  |-- START (windowId, ...) ----->|
  |<---------------- STARTED -----|
  |<=== UDP video frames =========|
  |-- GAMEPAD (on change) ------->|
  |-- PING / <-- PONG ----------->|   (latency probe, ~1 Hz)
  |-- STOP ---------------------->|
```

If the captured window closes, the host sends `ERROR` and stops the video
stream, but leaves the control connection open so the client can pick another
window from a fresh `WINDOW_LIST`.

## Deliberate omissions in v1

- **No encryption and no authentication.** This is a LAN protocol between two
  machines you own. Do not expose port 47810 to the internet.
- **No congestion control.** Bitrate is whatever the client asked for. On a wired
  or clean 5 GHz link this is fine; adaptive bitrate is future work.
- **No audio.** Video plus gamepad only, by scope.
