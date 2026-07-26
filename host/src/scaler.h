#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <winrt/base.h>

#include <memory>
#include <string>

namespace thorstream {

// GPU downscale from an arbitrary sub-rect of a source texture into a
// fixed-size destination. Streaming a 4K window to a 1080p handheld otherwise
// wastes ~4x the bandwidth and decode budget for no visible gain.
class TextureScaler {
public:
    ~TextureScaler();

    TextureScaler(const TextureScaler&) = delete;
    TextureScaler& operator=(const TextureScaler&) = delete;

    static std::unique_ptr<TextureScaler> Create(ID3D11Device* device,
                                                 ID3D11DeviceContext* context,
                                                 std::string* error);

    // Samples `sourceRect` of `source` and fills `destination` entirely.
    bool Draw(ID3D11Texture2D* source, const RECT& sourceRect, ID3D11Texture2D* destination);

    // Draws into a sub-rectangle of the destination instead of the whole thing,
    // leaving everything outside it untouched. Used to composite popups over an
    // already-drawn game frame.
    bool DrawInto(ID3D11Texture2D* source, const RECT& sourceRect, ID3D11Texture2D* destination,
                  const RECT& destinationRect);

private:
    struct Impl;
    TextureScaler();

    std::unique_ptr<Impl> impl_;
};

// Fits `sourceWidth` x `sourceHeight` inside a `maxWidth` x `maxHeight` box,
// preserving aspect ratio. Never upscales. Results are rounded down to even
// numbers, which H.264 requires. A zero max means "no limit on that axis".
void FitPreservingAspect(int sourceWidth, int sourceHeight, int maxWidth, int maxHeight,
                         int* outWidth, int* outHeight);

}  // namespace thorstream
