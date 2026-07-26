#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

#include "d3d_device.h"

namespace thorstream {

struct CaptureOptions {
    // Crop the captured surface down to the window's client area, dropping the
    // title bar and resize borders. Games running borderless are unaffected.
    bool cropToClientArea = true;
    bool captureCursor = false;
    // Suppress the yellow "this window is being captured" outline (Windows 11).
    bool drawBorder = false;
    // 0 = uncapped (deliver as fast as the compositor does).
    int maxFramesPerSecond = 0;
};

struct CapturedFrame {
    // Borrowed for the duration of the callback only. Do not retain.
    ID3D11Texture2D* texture = nullptr;
    // Region of `texture` that should actually be encoded.
    RECT crop{};
    int cropWidth = 0;
    int cropHeight = 0;
    // Compositor timestamp, 100ns units, comparable across frames.
    int64_t timestampHns = 0;
    uint64_t frameIndex = 0;
};

// Captures a single window via Windows.Graphics.Capture. The desktop is never
// part of the surface, so nothing behind or on top of the window can leak in.
class WindowCapture {
public:
    using FrameCallback = std::function<void(const CapturedFrame&)>;
    using ClosedCallback = std::function<void()>;

    WindowCapture(GraphicsDevice& device, HWND hwnd, const CaptureOptions& options);
    ~WindowCapture();

    WindowCapture(const WindowCapture&) = delete;
    WindowCapture& operator=(const WindowCapture&) = delete;

    void Start(FrameCallback onFrame, ClosedCallback onClosed);
    void Stop();

    // Size of the raw capture surface, before cropping.
    winrt::Windows::Graphics::SizeInt32 SurfaceSize() const { return surfaceSize_; }
    std::wstring DisplayName() const;

    // True if this build of Windows supports window capture at all.
    static bool IsSupported();

private:
    void OnFrameArrived(
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
        winrt::Windows::Foundation::IInspectable const& args);

    // Where the client area sits inside the capture surface. Recomputed per frame
    // because the window can move between monitors with different DPI.
    RECT ComputeCrop(const D3D11_TEXTURE2D_DESC& desc) const;

    GraphicsDevice& device_;
    HWND hwnd_;
    CaptureOptions options_;

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::SizeInt32 surfaceSize_{};

    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrived_;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem::Closed_revoker itemClosed_;

    FrameCallback onFrame_;
    ClosedCallback onClosed_;

    std::mutex mutex_;
    std::atomic<uint64_t> frameIndex_{0};
    std::atomic<bool> running_{false};
};

}  // namespace thorstream
