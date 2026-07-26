#include "scaler.h"

#include <d3dcompiler.h>

#include <algorithm>

namespace thorstream {
namespace {

// A fullscreen triangle rather than a quad: one primitive, no diagonal seam,
// and the source rect is passed as UVs so we never copy the frame first.
constexpr char kShaderSource[] = R"(
cbuffer Params : register(b0) { float4 srcRect; };  // u0, v0, u1, v1

struct VSOut {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    VSOut output;
    float2 t = float2((id << 1) & 2, id & 2);      // (0,0) (2,0) (0,2)
    output.position = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    output.uv = lerp(srcRect.xy, srcRect.zw, t);
    return output;
}

Texture2D    sourceTexture : register(t0);
SamplerState linearSampler : register(s0);

float4 PSMain(VSOut input) : SV_TARGET {
    return sourceTexture.Sample(linearSampler, input.uv);
}
)";

struct ScaleParams {
    float u0, v0, u1, v1;
};

winrt::com_ptr<ID3DBlob> Compile(const char* entry, const char* target, std::string* error) {
    winrt::com_ptr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "scaler.hlsl", nullptr,
                                  nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                  code.put(), errors.put());
    if (FAILED(hr)) {
        if (error) {
            *error = errors ? static_cast<const char*>(errors->GetBufferPointer())
                            : "shader compilation failed";
        }
        return nullptr;
    }
    return code;
}

}  // namespace

struct TextureScaler::Impl {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    winrt::com_ptr<ID3D11VertexShader> vertexShader;
    winrt::com_ptr<ID3D11PixelShader> pixelShader;
    winrt::com_ptr<ID3D11SamplerState> sampler;
    winrt::com_ptr<ID3D11Buffer> constants;

    // Views are rebuilt when the underlying texture changes, which happens on
    // every window resize.
    ID3D11Texture2D* cachedSource = nullptr;
    winrt::com_ptr<ID3D11ShaderResourceView> sourceView;
    ID3D11Texture2D* cachedDestination = nullptr;
    winrt::com_ptr<ID3D11RenderTargetView> destinationView;
};

TextureScaler::TextureScaler() : impl_(std::make_unique<Impl>()) {}
TextureScaler::~TextureScaler() = default;

std::unique_ptr<TextureScaler> TextureScaler::Create(ID3D11Device* device,
                                                     ID3D11DeviceContext* context,
                                                     std::string* error) {
    auto self = std::unique_ptr<TextureScaler>(new TextureScaler());
    auto& impl = *self->impl_;
    impl.device = device;
    impl.context = context;

    const auto vsCode = Compile("VSMain", "vs_5_0", error);
    if (!vsCode) return nullptr;
    const auto psCode = Compile("PSMain", "ps_5_0", error);
    if (!psCode) return nullptr;

    if (FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                                          nullptr, impl.vertexShader.put()))) {
        if (error) *error = "CreateVertexShader failed";
        return nullptr;
    }
    if (FAILED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(),
                                         nullptr, impl.pixelShader.put()))) {
        if (error) *error = "CreatePixelShader failed";
        return nullptr;
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&samplerDesc, impl.sampler.put()))) {
        if (error) *error = "CreateSamplerState failed";
        return nullptr;
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(ScaleParams);
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bufferDesc, nullptr, impl.constants.put()))) {
        if (error) *error = "constant buffer creation failed";
        return nullptr;
    }

    return self;
}

bool TextureScaler::Draw(ID3D11Texture2D* source, const RECT& sourceRect,
                         ID3D11Texture2D* destination) {
    auto& impl = *impl_;
    if (!source || !destination) return false;

    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    D3D11_TEXTURE2D_DESC destinationDesc{};
    destination->GetDesc(&destinationDesc);

    // The capture surface is not guaranteed to be sampleable; if it is not, the
    // caller has to fall back to a plain copy.
    if (!(sourceDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return false;

    if (impl.cachedSource != source) {
        impl.sourceView = nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        viewDesc.Format = sourceDesc.Format;
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipLevels = 1;
        if (FAILED(impl.device->CreateShaderResourceView(source, &viewDesc, impl.sourceView.put()))) {
            return false;
        }
        impl.cachedSource = source;
    }

    if (impl.cachedDestination != destination) {
        impl.destinationView = nullptr;
        if (FAILED(impl.device->CreateRenderTargetView(destination, nullptr,
                                                       impl.destinationView.put()))) {
            return false;
        }
        impl.cachedDestination = destination;
    }

    // Sample from texel centres so the edges of the source rect are not blended
    // with whatever lies just outside it.
    const float width = static_cast<float>(sourceDesc.Width);
    const float height = static_cast<float>(sourceDesc.Height);
    const ScaleParams params{
        (static_cast<float>(sourceRect.left) + 0.5f) / width,
        (static_cast<float>(sourceRect.top) + 0.5f) / height,
        (static_cast<float>(sourceRect.right) - 0.5f) / width,
        (static_cast<float>(sourceRect.bottom) - 0.5f) / height,
    };

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(impl.context->Map(impl.constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    memcpy(mapped.pData, &params, sizeof(params));
    impl.context->Unmap(impl.constants.get(), 0);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(destinationDesc.Width);
    viewport.Height = static_cast<float>(destinationDesc.Height);
    viewport.MaxDepth = 1.0f;

    ID3D11RenderTargetView* renderTargets[] = {impl.destinationView.get()};
    ID3D11ShaderResourceView* resources[] = {impl.sourceView.get()};
    ID3D11SamplerState* samplers[] = {impl.sampler.get()};
    ID3D11Buffer* buffers[] = {impl.constants.get()};

    impl.context->IASetInputLayout(nullptr);
    impl.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    impl.context->VSSetShader(impl.vertexShader.get(), nullptr, 0);
    impl.context->VSSetConstantBuffers(0, 1, buffers);
    impl.context->PSSetShader(impl.pixelShader.get(), nullptr, 0);
    impl.context->PSSetShaderResources(0, 1, resources);
    impl.context->PSSetSamplers(0, 1, samplers);
    impl.context->RSSetViewports(1, &viewport);
    impl.context->OMSetRenderTargets(1, renderTargets, nullptr);
    impl.context->Draw(3, 0);

    // Unbind so the encoder can take the texture without a hazard warning.
    ID3D11ShaderResourceView* none[] = {nullptr};
    impl.context->PSSetShaderResources(0, 1, none);
    ID3D11RenderTargetView* noTargets[] = {nullptr};
    impl.context->OMSetRenderTargets(1, noTargets, nullptr);
    return true;
}

void FitPreservingAspect(int sourceWidth, int sourceHeight, int maxWidth, int maxHeight,
                         int* outWidth, int* outHeight) {
    int width = sourceWidth;
    int height = sourceHeight;

    if (sourceWidth > 0 && sourceHeight > 0 && (maxWidth > 0 || maxHeight > 0)) {
        // A zero on either axis means unconstrained, so treat it as infinite
        // rather than clamping the picture to nothing.
        const double limitX = maxWidth > 0 ? static_cast<double>(maxWidth) / sourceWidth : 1e9;
        const double limitY = maxHeight > 0 ? static_cast<double>(maxHeight) / sourceHeight : 1e9;
        const double scale = std::min({limitX, limitY, 1.0});  // never upscale

        width = static_cast<int>(sourceWidth * scale);
        height = static_cast<int>(sourceHeight * scale);
    }

    *outWidth = std::max(width & ~1, 2);
    *outHeight = std::max(height & ~1, 2);
}

}  // namespace thorstream
