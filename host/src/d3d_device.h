#pragma once

#include <d3d11.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

namespace thorstream {

// A hardware D3D11 device plus its WinRT projection, which is what the capture
// frame pool needs. Both views refer to the same underlying device, so textures
// handed to us by the capture API can be used directly by the encoder.
struct GraphicsDevice {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice{nullptr};
};

GraphicsDevice CreateGraphicsDevice();

// Unwraps the ID3D11Texture2D backing a captured frame's surface.
winrt::com_ptr<ID3D11Texture2D> GetTextureFromSurface(
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface const& surface);

}  // namespace thorstream
