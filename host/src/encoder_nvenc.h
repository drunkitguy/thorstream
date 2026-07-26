#pragma once

#include <Windows.h>
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace thorstream {

struct EncoderSettings {
    int width = 1920;
    int height = 1080;
    int framerate = 60;
    int bitrateKbps = 20000;
    // H.264 is the safest bet for decode latency on Android handhelds. HEVC
    // halves the bitrate for the same quality but costs a few ms to decode.
    bool useHevc = false;
};

struct EncodedPacket {
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool isKeyframe = false;
    uint64_t timestamp = 0;
};

// Hardware H.264/HEVC encoder driven straight off a D3D11 texture. The NVENC
// entry points are resolved from the display driver at runtime, so no NVIDIA SDK
// is needed to build and the binary still loads on machines without an NVIDIA GPU
// (Create() simply fails, and the caller can fall back).
class NvencEncoder {
public:
    using PacketCallback = std::function<void(const EncodedPacket&)>;

    ~NvencEncoder();

    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    // Returns nullptr and fills `error` if NVENC is unavailable or unsupported.
    static std::unique_ptr<NvencEncoder> Create(ID3D11Device* device,
                                                ID3D11DeviceContext* context,
                                                const EncoderSettings& settings,
                                                std::string* error);

    // Copies `crop` out of `texture` and encodes it. The callback fires
    // synchronously with the resulting bitstream, still owned by the encoder.
    bool EncodeFrame(ID3D11Texture2D* texture, const RECT& crop, uint64_t timestamp,
                     const PacketCallback& onPacket);

    // Ask for an IDR on the next frame - used when a client joins or reports loss.
    void RequestKeyframe() { forceIdr_ = true; }

    // Re-encode the previous frame. Windows.Graphics.Capture only delivers frames
    // when the window actually changes, so a still image would otherwise stall the
    // client's decoder; this keeps the stream ticking over.
    bool RepeatLastFrame(uint64_t timestamp, const PacketCallback& onPacket);

    void UpdateBitrate(int bitrateKbps);

    int Width() const { return settings_.width; }
    int Height() const { return settings_.height; }

    // SPS/PPS (or VPS/SPS/PPS), for clients that want out-of-band config.
    const std::vector<uint8_t>& SequenceHeader() const { return sequenceHeader_; }

private:
    struct Impl;

    NvencEncoder();

    std::unique_ptr<Impl> impl_;
    EncoderSettings settings_{};
    std::vector<uint8_t> sequenceHeader_;
    bool forceIdr_ = true;
    bool hasFrame_ = false;
};

}  // namespace thorstream
