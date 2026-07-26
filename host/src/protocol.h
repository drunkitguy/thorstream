#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Wire format shared by the host and the Android client. See PROTOCOL.md.
namespace thorstream::protocol {

inline constexpr uint16_t kVersion = 1;
inline constexpr uint16_t kDefaultControlPort = 47810;
inline constexpr uint16_t kDefaultVideoPort = 47811;

// Keep datagrams under a 1280-byte MTU so no LAN path IP-fragments them; one
// lost IP fragment would otherwise destroy the whole UDP packet.
inline constexpr size_t kMaxDatagram = 1280;
inline constexpr uint32_t kVideoMagic = 0x31565354;  // 'TSV1'

enum class MessageType : uint8_t {
    Hello = 0x01,
    WindowList = 0x02,
    Start = 0x03,
    Started = 0x04,
    Stop = 0x05,
    RequestIdr = 0x06,
    Gamepad = 0x07,
    Ping = 0x08,
    Pong = 0x09,
    Error = 0x0A,

    // The client picks a game from the user's Playnite library rather than from
    // whatever windows happen to be open.
    GameList = 0x0B,       // host -> client
    Launch = 0x0C,         // client -> host
    LaunchProgress = 0x0D, // host -> client: games take a while to start

    // Cover art is fetched lazily, one game at a time, rather than bundled into
    // the game list: full-size artwork is over a megabyte per title.
    CoverRequest = 0x0E,  // client -> host: game id
    CoverData = 0x0F,     // host -> client: game id, then JPEG bytes (empty if none)

    // Touchscreen and on-screen keyboard, all client -> host.
    MouseMove = 0x10,    // uint16 x, uint16 y, normalised across the desktop
    MouseButton = 0x11,  // uint8 button, uint8 pressed
    MouseScroll = 0x12,  // int16 delta, in wheel notches
    Key = 0x13,          // uint16 virtual key, uint8 pressed
    Text = 0x14,         // string, typed as Unicode
};

// Covers are scaled to this width before sending. Big enough for a tile on a
// 1080p handheld, small enough that a whole library is a few megabytes.
inline constexpr int kCoverWidth = 342;

enum class Codec : uint8_t { H264 = 0, Hevc = 1 };

#pragma pack(push, 1)
struct VideoFragmentHeader {
    uint32_t magic;
    uint32_t frameNumber;
    uint16_t fragmentIndex;
    uint16_t fragmentCount;
    uint64_t timestampMicros;
    uint8_t flags;
    uint8_t reserved;
    uint16_t payloadSize;
};
static_assert(sizeof(VideoFragmentHeader) == 24, "video header must stay 24 bytes");

struct GamepadState {
    uint16_t buttons;
    uint8_t leftTrigger;
    uint8_t rightTrigger;
    int16_t leftStickX;
    int16_t leftStickY;
    int16_t rightStickX;
    int16_t rightStickY;
    uint32_t sequence;
};
static_assert(sizeof(GamepadState) == 16, "gamepad state must stay 16 bytes");
#pragma pack(pop)

inline constexpr uint8_t kFlagKeyframe = 0x01;
inline constexpr size_t kMaxVideoPayload = kMaxDatagram - sizeof(VideoFragmentHeader);

// ---- little-endian buffer helpers -------------------------------------------
// x86 and ARM are both little-endian, so these are memcpy in practice, but being
// explicit keeps the wire format independent of whatever compiles it.

class Writer {
public:
    void U8(uint8_t v) { bytes_.push_back(v); }
    void U16(uint16_t v) { Raw(&v, sizeof(v)); }
    void U32(uint32_t v) { Raw(&v, sizeof(v)); }
    void U64(uint64_t v) { Raw(&v, sizeof(v)); }

    void Str(const std::string& s) {
        U16(static_cast<uint16_t>(s.size()));
        Raw(s.data(), s.size());
    }

    void Raw(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        bytes_.insert(bytes_.end(), p, p + size);
    }

    const std::vector<uint8_t>& Bytes() const { return bytes_; }
    size_t Size() const { return bytes_.size(); }

private:
    std::vector<uint8_t> bytes_;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool U8(uint8_t& out) { return Raw(&out, sizeof(out)); }
    bool U16(uint16_t& out) { return Raw(&out, sizeof(out)); }
    bool U32(uint32_t& out) { return Raw(&out, sizeof(out)); }
    bool U64(uint64_t& out) { return Raw(&out, sizeof(out)); }

    bool Str(std::string& out) {
        uint16_t length = 0;
        if (!U16(length)) return false;
        if (Remaining() < length) return false;
        out.assign(reinterpret_cast<const char*>(data_ + offset_), length);
        offset_ += length;
        return true;
    }

    bool Raw(void* out, size_t size) {
        if (Remaining() < size) return false;
        memcpy(out, data_ + offset_, size);
        offset_ += size;
        return true;
    }

    size_t Remaining() const { return size_ - offset_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t offset_ = 0;
};

}  // namespace thorstream::protocol
