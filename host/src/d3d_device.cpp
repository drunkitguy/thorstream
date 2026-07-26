#include "d3d_device.h"

#include <d3d11_4.h>  // ID3D11Multithread
#include <dxgi.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace thorstream {

GraphicsDevice CreateGraphicsDevice() {
    GraphicsDevice result;

    // BGRA support is required to interop with the capture API and with D2D.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    // Only ask for the debug layer if the developer runtime is actually present.
    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    flags | D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                                    D3D11_SDK_VERSION, nullptr, nullptr, nullptr))) {
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    }
#endif

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

    winrt::check_hresult(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, result.device.put(), nullptr, result.context.put()));

    // The capture frame pool is fed from multiple threads; D3D11 device contexts
    // are not thread safe, so let the runtime serialise access for us.
    if (auto multithread = result.device.try_as<ID3D11Multithread>()) {
        multithread->SetMultithreadProtected(TRUE);
    }

    const auto dxgiDevice = result.device.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    result.winrtDevice =
        inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

    return result;
}

winrt::com_ptr<ID3D11Texture2D> GetTextureFromSurface(
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface const& surface) {
    const auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), texture.put_void()));
    return texture;
}

}  // namespace thorstream
