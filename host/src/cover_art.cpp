#include "cover_art.h"

#include <Windows.h>
#include <wincodec.h>
#include <winrt/base.h>

namespace thorstream {

bool CoverArt::LoadThumbnail(const std::wstring& path, int maxWidth,
                             std::vector<uint8_t>* jpegOut) {
    if (path.empty() || !jpegOut) return false;

    winrt::com_ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.put())))) {
        return false;
    }

    // Playnite does not always give cover files a meaningful extension, so let
    // WIC sniff the contents rather than trusting the name.
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand,
                                                  decoder.put()))) {
        return false;
    }

    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return false;

    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) return false;

    UINT targetWidth = width;
    UINT targetHeight = height;
    if (maxWidth > 0 && width > static_cast<UINT>(maxWidth)) {
        targetWidth = static_cast<UINT>(maxWidth);
        targetHeight = static_cast<UINT>(
            static_cast<uint64_t>(height) * targetWidth / width);
        if (targetHeight == 0) targetHeight = 1;
    }

    winrt::com_ptr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(scaler.put()))) return false;
    if (FAILED(scaler->Initialize(frame.get(), targetWidth, targetHeight,
                                  WICBitmapInterpolationModeFant))) {
        return false;
    }

    // JPEG cannot carry alpha, and the encoder rejects a source that has it.
    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.put()))) return false;
    if (FAILED(converter->Initialize(scaler.get(), GUID_WICPixelFormat24bppBGR,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }

    winrt::com_ptr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()))) return false;

    winrt::com_ptr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.put()))) return false;
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;

    winrt::com_ptr<IWICBitmapFrameEncode> encodeFrame;
    winrt::com_ptr<IPropertyBag2> options;
    if (FAILED(encoder->CreateNewFrame(encodeFrame.put(), options.put()))) return false;

    // Quality is a visible-vs-bytes trade; 0.8 keeps box art clean at tile size.
    if (options) {
        PROPBAG2 quality{};
        quality.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value{};
        value.vt = VT_R4;
        value.fltVal = 0.8f;
        options->Write(1, &quality, &value);
    }

    if (FAILED(encodeFrame->Initialize(options.get()))) return false;
    if (FAILED(encodeFrame->SetSize(targetWidth, targetHeight))) return false;

    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(encodeFrame->SetPixelFormat(&format))) return false;
    if (FAILED(encodeFrame->WriteSource(converter.get(), nullptr))) return false;
    if (FAILED(encodeFrame->Commit()) || FAILED(encoder->Commit())) return false;

    HGLOBAL memory = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.get(), &memory)) || !memory) return false;

    const SIZE_T size = GlobalSize(memory);
    const void* data = GlobalLock(memory);
    if (!data || size == 0) return false;

    jpegOut->assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    GlobalUnlock(memory);
    return true;
}

}  // namespace thorstream
