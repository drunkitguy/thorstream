#include "save_png.h"

#include <wincodec.h>
#include <winrt/base.h>

#include <vector>

namespace thorstream {

bool SaveTextureRegionAsPng(ID3D11Device* device, ID3D11DeviceContext* context,
                            ID3D11Texture2D* texture, const RECT& crop,
                            const std::wstring& path) {
    const UINT width = static_cast<UINT>(crop.right - crop.left);
    const UINT height = static_cast<UINT>(crop.bottom - crop.top);
    if (width == 0 || height == 0) return false;

    D3D11_TEXTURE2D_DESC sourceDesc{};
    texture->GetDesc(&sourceDesc);

    D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
    stagingDesc.Width = width;
    stagingDesc.Height = height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, staging.put()))) return false;

    const D3D11_BOX box{static_cast<UINT>(crop.left),  static_cast<UINT>(crop.top),    0,
                        static_cast<UINT>(crop.right), static_cast<UINT>(crop.bottom), 1};
    context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, texture, 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

    // Copy out immediately: WIC must not hold a mapped D3D resource.
    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
    for (UINT y = 0; y < height; ++y) {
        memcpy(pixels.data() + static_cast<size_t>(y) * width * 4,
               static_cast<const BYTE*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
               static_cast<size_t>(width) * 4);
    }
    context->Unmap(staging.get(), 0);

    winrt::com_ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.put())))) {
        return false;
    }

    winrt::com_ptr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.put()))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;

    winrt::com_ptr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()))) return false;
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;

    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::com_ptr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(frame.put(), props.put()))) return false;
    if (FAILED(frame->Initialize(props.get()))) return false;
    if (FAILED(frame->SetSize(width, height))) return false;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    if (FAILED(frame->WritePixels(height, width * 4, static_cast<UINT>(pixels.size()),
                                  pixels.data()))) {
        return false;
    }
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
}

}  // namespace thorstream
