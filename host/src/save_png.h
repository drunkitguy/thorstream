#pragma once

#include <Windows.h>
#include <d3d11.h>

#include <string>

namespace thorstream {

// Copies `crop` out of `texture` into a staging texture and writes it as a PNG.
// Debug/verification helper - not on the streaming path.
bool SaveTextureRegionAsPng(ID3D11Device* device, ID3D11DeviceContext* context,
                            ID3D11Texture2D* texture, const RECT& crop,
                            const std::wstring& path);

}  // namespace thorstream
