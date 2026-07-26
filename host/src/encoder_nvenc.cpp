#include "encoder_nvenc.h"

#include <winrt/base.h>

#include <algorithm>
#include <cstring>

#include "ffnvcodec/nvEncodeAPI.h"
#include "scaler.h"

namespace thorstream {
namespace {

using NvEncodeAPICreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using NvEncodeAPIGetMaxSupportedVersionFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);

// Without this the bitstream carries no colour information at all and every
// decoder falls back to its own default. Measured: NVENC converts our BGRA input
// using BT.709 with studio (limited) range, so that is what we declare. Getting
// this wrong is subtle rather than obvious - the picture still appears, it is
// just the wrong colour: full-range decoding of limited-range data washes out
// blacks, and BT.601 decoding of BT.709 data shifts hues.
void FillVuiParameters(NV_ENC_CONFIG_H264_VUI_PARAMETERS& vui, bool fullRange) {
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    vui.videoFullRangeFlag = fullRange ? 1 : 0;

    vui.colourDescriptionPresentFlag = 1;
    vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
}

std::string StatusText(NVENCSTATUS status) {
    switch (status) {
        case NV_ENC_SUCCESS: return "success";
        case NV_ENC_ERR_NO_ENCODE_DEVICE: return "no encode device";
        case NV_ENC_ERR_UNSUPPORTED_DEVICE: return "unsupported device";
        case NV_ENC_ERR_INVALID_VERSION: return "driver too old for this NVENC header";
        case NV_ENC_ERR_INVALID_PARAM: return "invalid parameter";
        case NV_ENC_ERR_OUT_OF_MEMORY: return "out of memory";
        case NV_ENC_ERR_UNSUPPORTED_PARAM: return "unsupported parameter";
        case NV_ENC_ERR_ENCODER_BUSY: return "encoder busy";
        default: return "NVENC error " + std::to_string(static_cast<int>(status));
    }
}

}  // namespace

struct NvencEncoder::Impl {
    HMODULE library = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api{};
    void* encoder = nullptr;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    // Kept alive because nvEncReconfigureEncoder needs the full original params,
    // and because initParams.encodeConfig points at `config`.
    NV_ENC_INITIALIZE_PARAMS initParams{};
    NV_ENC_CONFIG config{};

    // We own a texture sized to the crop region; the capture surface itself is
    // the whole window and can change size underneath us.
    winrt::com_ptr<ID3D11Texture2D> inputTexture;
    std::unique_ptr<TextureScaler> scaler;
    NV_ENC_REGISTERED_PTR registeredInput = nullptr;
    NV_ENC_OUTPUT_PTR bitstream = nullptr;

    ~Impl() {
        if (encoder) {
            if (registeredInput && api.nvEncUnregisterResource) {
                api.nvEncUnregisterResource(encoder, registeredInput);
            }
            if (bitstream && api.nvEncDestroyBitstreamBuffer) {
                api.nvEncDestroyBitstreamBuffer(encoder, bitstream);
            }
            if (api.nvEncDestroyEncoder) api.nvEncDestroyEncoder(encoder);
        }
        if (library) FreeLibrary(library);
    }
};

NvencEncoder::NvencEncoder() : impl_(std::make_unique<Impl>()) {}
NvencEncoder::~NvencEncoder() = default;

std::unique_ptr<NvencEncoder> NvencEncoder::Create(ID3D11Device* device,
                                                   ID3D11DeviceContext* context,
                                                   const EncoderSettings& settings,
                                                   std::string* error) {
    const auto fail = [&](std::string message) -> std::unique_ptr<NvencEncoder> {
        if (error) *error = std::move(message);
        return nullptr;
    };

    if (settings.width <= 0 || settings.height <= 0) return fail("invalid encoder dimensions");

    auto self = std::unique_ptr<NvencEncoder>(new NvencEncoder());
    self->settings_ = settings;
    auto& impl = *self->impl_;
    impl.device = device;
    impl.context = context;

    impl.library = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!impl.library) return fail("nvEncodeAPI64.dll not found (no NVIDIA driver?)");

    auto getMaxVersion = reinterpret_cast<NvEncodeAPIGetMaxSupportedVersionFn>(
        GetProcAddress(impl.library, "NvEncodeAPIGetMaxSupportedVersion"));
    auto createInstance = reinterpret_cast<NvEncodeAPICreateInstanceFn>(
        GetProcAddress(impl.library, "NvEncodeAPICreateInstance"));
    if (!getMaxVersion || !createInstance) return fail("nvEncodeAPI64.dll is missing exports");

    // The driver refuses to talk to a header newer than it understands, so check
    // before we hand it a version it will only reject with a vague error.
    uint32_t driverVersion = 0;
    if (getMaxVersion(&driverVersion) != NV_ENC_SUCCESS) return fail("NvEncodeAPIGetMaxSupportedVersion failed");
    const uint32_t headerVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (driverVersion < headerVersion) {
        return fail("NVIDIA driver is too old for NVENC API " +
                    std::to_string(NVENCAPI_MAJOR_VERSION) + "." +
                    std::to_string(NVENCAPI_MINOR_VERSION) + " - please update it");
    }

    impl.api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (const auto status = createInstance(&impl.api); status != NV_ENC_SUCCESS) {
        return fail("NvEncodeAPICreateInstance: " + StatusText(status));
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session{};
    session.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    session.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    session.device = device;
    session.apiVersion = NVENCAPI_VERSION;
    if (const auto status = impl.api.nvEncOpenEncodeSessionEx(&session, &impl.encoder);
        status != NV_ENC_SUCCESS) {
        return fail("nvEncOpenEncodeSessionEx: " + StatusText(status));
    }

    const GUID codec = settings.useHevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
    // P1 is the fastest preset; combined with the ultra-low-latency tuning this is
    // the configuration Moonlight-style streaming wants.
    const GUID preset = NV_ENC_PRESET_P1_GUID;

    NV_ENC_PRESET_CONFIG presetConfig{};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
    if (const auto status = impl.api.nvEncGetEncodePresetConfigEx(
            impl.encoder, codec, preset, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &presetConfig);
        status != NV_ENC_SUCCESS) {
        return fail("nvEncGetEncodePresetConfigEx: " + StatusText(status));
    }

    NV_ENC_CONFIG& config = impl.config;
    config = presetConfig.presetCfg;
    config.version = NV_ENC_CONFIG_VER;
    config.gopLength = NVENC_INFINITE_GOPLENGTH;  // keyframes on demand only
    config.frameIntervalP = 1;                    // no B-frames: they add a frame of latency

    config.rcParams.version = NV_ENC_RC_PARAMS_VER;
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config.rcParams.averageBitRate = static_cast<uint32_t>(settings.bitrateKbps) * 1000u;
    config.rcParams.maxBitRate = config.rcParams.averageBitRate;
    // A one-frame VBV is what keeps a CBR stream from bursting past the link and
    // adding queueing delay - the single biggest latency win in the whole config.
    config.rcParams.vbvBufferSize =
        config.rcParams.averageBitRate / static_cast<uint32_t>(std::max(settings.framerate, 1));
    config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
    config.rcParams.enableAQ = 1;

    if (settings.useHevc) {
        auto& hevc = config.encodeCodecConfig.hevcConfig;
        hevc.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        hevc.repeatSPSPPS = 1;
        hevc.outputAUD = 0;
        FillVuiParameters(hevc.hevcVUIParameters, settings.fullRange);
    } else {
        auto& h264 = config.encodeCodecConfig.h264Config;
        h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        h264.repeatSPSPPS = 1;  // client can join mid-stream without a side channel
        h264.outputAUD = 0;
        FillVuiParameters(h264.h264VUIParameters, settings.fullRange);
        h264.sliceMode = 3;      // slices per frame...
        h264.sliceModeData = 1;  // ...one, so a frame is one NAL unit
    }

    NV_ENC_INITIALIZE_PARAMS& init = impl.initParams;
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = codec;
    init.presetGUID = preset;
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init.encodeWidth = static_cast<uint32_t>(settings.width);
    init.encodeHeight = static_cast<uint32_t>(settings.height);
    init.darWidth = static_cast<uint32_t>(settings.width);
    init.darHeight = static_cast<uint32_t>(settings.height);
    init.frameRateNum = static_cast<uint32_t>(std::max(settings.framerate, 1));
    init.frameRateDen = 1;
    init.enablePTD = 1;
    init.encodeConfig = &config;

    if (const auto status = impl.api.nvEncInitializeEncoder(impl.encoder, &init);
        status != NV_ENC_SUCCESS) {
        return fail("nvEncInitializeEncoder: " + StatusText(status));
    }

    // Input texture, sized to the crop we will be encoding.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(settings.width);
    desc.Height = static_cast<UINT>(settings.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, impl.inputTexture.put()))) {
        return fail("failed to create the encoder input texture");
    }

    // Only needed when the encode size differs from the window, but building it
    // up front means a resize never fails mid-stream.
    std::string scalerError;
    impl.scaler = TextureScaler::Create(device, context, &scalerError);
    if (!impl.scaler) return fail("scaler: " + scalerError);

    NV_ENC_REGISTER_RESOURCE reg{};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.width = static_cast<uint32_t>(settings.width);
    reg.height = static_cast<uint32_t>(settings.height);
    reg.pitch = 0;
    reg.resourceToRegister = impl.inputTexture.get();
    // NVENC's "ARGB" is a 32-bit word with B in the low byte: DXGI's BGRA.
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    if (const auto status = impl.api.nvEncRegisterResource(impl.encoder, &reg);
        status != NV_ENC_SUCCESS) {
        return fail("nvEncRegisterResource: " + StatusText(status));
    }
    impl.registeredInput = reg.registeredResource;

    NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
    bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (const auto status = impl.api.nvEncCreateBitstreamBuffer(impl.encoder, &bitstream);
        status != NV_ENC_SUCCESS) {
        return fail("nvEncCreateBitstreamBuffer: " + StatusText(status));
    }
    impl.bitstream = bitstream.bitstreamBuffer;

    // Pull the sequence header out up front so clients can be configured before
    // the first packet arrives.
    uint8_t headerBuffer[1024] = {};
    uint32_t headerSize = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload{};
    payload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    payload.inBufferSize = sizeof(headerBuffer);
    payload.spsppsBuffer = headerBuffer;
    payload.outSPSPPSPayloadSize = &headerSize;
    if (impl.api.nvEncGetSequenceParams(impl.encoder, &payload) == NV_ENC_SUCCESS) {
        self->sequenceHeader_.assign(headerBuffer, headerBuffer + headerSize);
    }

    return self;
}

bool NvencEncoder::EncodeFrame(ID3D11Texture2D* texture, const RECT& crop, uint64_t timestamp,
                               const PacketCallback& onPacket,
                               const std::vector<Overlay>& overlays) {
    auto& impl = *impl_;
    if (!impl.encoder) return false;

    const UINT cropW = static_cast<UINT>(crop.right - crop.left);
    const UINT cropH = static_cast<UINT>(crop.bottom - crop.top);
    if (cropW == 0 || cropH == 0) return false;

    const bool sizeMatches = cropW == static_cast<UINT>(settings_.width) &&
                             cropH == static_cast<UINT>(settings_.height);

    if (sizeMatches) {
        // Exact size: a straight copy beats resampling identical pixels.
        const D3D11_BOX box{static_cast<UINT>(crop.left),
                            static_cast<UINT>(crop.top),
                            0,
                            static_cast<UINT>(crop.right),
                            static_cast<UINT>(crop.bottom),
                            1};
        impl.context->CopySubresourceRegion(impl.inputTexture.get(), 0, 0, 0, 0, texture, 0, &box);
    } else if (!impl.scaler || !impl.scaler->Draw(texture, crop, impl.inputTexture.get())) {
        // Falling back to a crop here would silently show a corner of the window
        // rather than the whole thing, so refuse the frame instead.
        return false;
    }

    // Composite anything that should appear over the game, scaled by the same
    // factor as the frame beneath it so a dialog lands where it actually sits.
    if (!overlays.empty() && impl.scaler) {
        const double scaleX = static_cast<double>(settings_.width) / (std::max)(cropW, 1u);
        const double scaleY = static_cast<double>(settings_.height) / (std::max)(cropH, 1u);

        for (const auto& overlay : overlays) {
            if (!overlay.texture) continue;

            RECT target;
            target.left = static_cast<LONG>(overlay.destination.left * scaleX);
            target.top = static_cast<LONG>(overlay.destination.top * scaleY);
            target.right = static_cast<LONG>(overlay.destination.right * scaleX);
            target.bottom = static_cast<LONG>(overlay.destination.bottom * scaleY);

            impl.scaler->DrawInto(overlay.texture, overlay.source, impl.inputTexture.get(), target);
        }
    }

    hasFrame_ = true;
    return RepeatLastFrame(timestamp, onPacket);
}

bool NvencEncoder::RepeatLastFrame(uint64_t timestamp, const PacketCallback& onPacket) {
    auto& impl = *impl_;
    if (!impl.encoder || !hasFrame_) return false;

    NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = impl.registeredInput;
    if (impl.api.nvEncMapInputResource(impl.encoder, &map) != NV_ENC_SUCCESS) return false;

    NV_ENC_PIC_PARAMS pic{};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.inputWidth = static_cast<uint32_t>(settings_.width);
    pic.inputHeight = static_cast<uint32_t>(settings_.height);
    pic.outputBitstream = impl.bitstream;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = timestamp;
    if (forceIdr_) {
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }

    const NVENCSTATUS status = impl.api.nvEncEncodePicture(impl.encoder, &pic);
    const bool wasIdr = forceIdr_;
    if (status == NV_ENC_SUCCESS) forceIdr_ = false;

    bool delivered = false;
    if (status == NV_ENC_SUCCESS) {
        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = impl.bitstream;
        lock.doNotWait = 0;
        if (impl.api.nvEncLockBitstream(impl.encoder, &lock) == NV_ENC_SUCCESS) {
            EncodedPacket packet;
            packet.data = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
            packet.size = lock.bitstreamSizeInBytes;
            packet.isKeyframe = wasIdr || lock.pictureType == NV_ENC_PIC_TYPE_IDR ||
                                lock.pictureType == NV_ENC_PIC_TYPE_I;
            packet.timestamp = lock.outputTimeStamp;
            if (onPacket) onPacket(packet);
            delivered = true;
            impl.api.nvEncUnlockBitstream(impl.encoder, impl.bitstream);
        }
    }

    impl.api.nvEncUnmapInputResource(impl.encoder, map.mappedResource);
    return delivered;
}

void NvencEncoder::UpdateBitrate(int bitrateKbps) {
    auto& impl = *impl_;
    if (!impl.encoder || bitrateKbps <= 0 || bitrateKbps == settings_.bitrateKbps) return;

    const auto bitsPerSecond = static_cast<uint32_t>(bitrateKbps) * 1000u;
    impl.config.rcParams.averageBitRate = bitsPerSecond;
    impl.config.rcParams.maxBitRate = bitsPerSecond;
    impl.config.rcParams.vbvBufferSize =
        bitsPerSecond / static_cast<uint32_t>(std::max(settings_.framerate, 1));
    impl.config.rcParams.vbvInitialDelay = impl.config.rcParams.vbvBufferSize;

    NV_ENC_RECONFIGURE_PARAMS params{};
    params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
    params.reInitEncodeParams = impl.initParams;  // still points at impl.config
    params.resetEncoder = 0;
    params.forceIDR = 1;

    if (impl.api.nvEncReconfigureEncoder(impl.encoder, &params) == NV_ENC_SUCCESS) {
        settings_.bitrateKbps = bitrateKbps;
    }
}

}  // namespace thorstream
