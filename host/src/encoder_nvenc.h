#pragma once

#include <Windows.h>
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace thorstream {

// Deliberately not protocol::Codec: the encoder has no business knowing the wire
// format. The values line up anyway, but callers convert explicitly so that a
// future wire codec cannot silently become the wrong encoder GUID.
enum class VideoCodec { H264, Hevc, Av1 };

struct EncoderSettings {
    int width = 1920;
    int height = 1080;
    int framerate = 60;
    int bitrateKbps = 20000;
    // H.264 is the safest bet for decode latency on Android handhelds. HEVC
    // halves the bitrate for the same quality but costs a few ms to decode.
    // AV1 saves another 30-40% on top of HEVC, which matters most at the low
    // bitrates a handheld over Wi-Fi actually gets, but needs an Ada-generation
    // GPU to encode and a recent SoC to decode.
    VideoCodec codec = VideoCodec::H264;
    // Declared in the bitstream's VUI so the decoder does not have to guess.
    // Must match what NVENC actually produces - a wrong declaration is worse
    // than none, because decoders trust it.
    bool fullRange = false;
};

struct EncodedPacket {
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool isKeyframe = false;
    uint64_t timestamp = 0;
};

// Hardware H.264/HEVC/AV1 encoder driven straight off a D3D11 texture. The NVENC
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

    // An extra image drawn over the frame, e.g. a dialog that appeared over the
    // game. Positions are in the source's coordinate space and are scaled along
    // with everything else.
    struct Overlay {
        ID3D11Texture2D* texture = nullptr;
        RECT source{};
        RECT destination{};  // relative to `crop`
    };

    // Copies `crop` out of `texture` and encodes it. The callback fires
    // synchronously with the resulting bitstream, still owned by the encoder.
    bool EncodeFrame(ID3D11Texture2D* texture, const RECT& crop, uint64_t timestamp,
                     const PacketCallback& onPacket,
                     const std::vector<Overlay>& overlays = {});

    // Ask for an IDR on the next frame - used when a client joins or reports loss.
    void RequestKeyframe() { forceIdr_ = true; }

    // Re-encode the previous frame. Windows.Graphics.Capture only delivers frames
    // when the window actually changes, so a still image would otherwise stall the
    // client's decoder; this keeps the stream ticking over.
    bool RepeatLastFrame(uint64_t timestamp, const PacketCallback& onPacket);

    void UpdateBitrate(int bitrateKbps);

    int Width() const { return settings_.width; }
    int Height() const { return settings_.height; }

    // Out-of-band decoder configuration, exactly as NVENC hands it over:
    //   H.264 - Annex-B SPS then PPS (start codes included)
    //   HEVC  - Annex-B VPS, SPS, PPS
    //   AV1   - a bare OBU_SEQUENCE_HEADER in low-overhead format (obu_has_size_
    //           field = 1), NOT an AV1CodecConfigurationRecord / "av1C" box.
    //           Measured as the whole payload and nothing else at both 1080p (16
    //           bytes, obu_size 14) and 4K (17 bytes, obu_size 15), so it can be
    //           used unmodified as the configOBUs field of an av1C record.
    // Note this is not the same shape as a packet from EncodeFrame: those come
    // from nvEncLockBitstream and are complete temporal units, which per the AV1
    // spec must open with a temporal delimiter. That TD belongs to the frame, not
    // to this field.
    // Repeated in-band on every keyframe as well, so a client may ignore this.
    const std::vector<uint8_t>& SequenceHeader() const { return sequenceHeader_; }

    // For AV1, the profile/level/tier the encoder actually chose, read back out
    // of the sequence header: "av01.0.09H (level 4.1, High tier)". Empty for
    // H.264 and HEVC. Worth logging because NVENC derives level and tier from
    // the bitrate, so it is not knowable until the encoder has been initialised,
    // and High tier is the difference between a picture and a black screen on
    // some Android SoCs.
    const std::string& BitstreamDescription() const { return bitstreamDescription_; }

private:
    struct Impl;

    NvencEncoder();

    std::unique_ptr<Impl> impl_;
    EncoderSettings settings_{};
    std::vector<uint8_t> sequenceHeader_;
    std::string bitstreamDescription_;
    bool forceIdr_ = true;
    bool hasFrame_ = false;
};

}  // namespace thorstream
