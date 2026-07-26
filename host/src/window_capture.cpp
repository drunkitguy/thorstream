#include "window_capture.h"

#include <dwmapi.h>
#include <windows.graphics.capture.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <algorithm>

namespace winrt {
using namespace Windows::Foundation;
using namespace Windows::Foundation::Metadata;
using namespace Windows::Graphics;
using namespace Windows::Graphics::Capture;
using namespace Windows::Graphics::DirectX;
using namespace Windows::Graphics::DirectX::Direct3D11;
}  // namespace winrt

namespace thorstream {
namespace {

constexpr auto kCaptureSessionClass = L"Windows.Graphics.Capture.GraphicsCaptureSession";

bool ApiPresent(const wchar_t* typeName, const wchar_t* propertyName) {
    return winrt::ApiInformation::IsPropertyPresent(typeName, propertyName);
}

winrt::GraphicsCaptureItem CreateItemForWindow(HWND hwnd) {
    const auto factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem,
                                                       ::IGraphicsCaptureItemInterop>();
    winrt::GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(factory->CreateForWindow(
        hwnd, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
        winrt::put_abi(item)));
    return item;
}

}  // namespace

bool WindowCapture::IsSupported() {
    return winrt::GraphicsCaptureSession::IsSupported();
}

WindowCapture::WindowCapture(GraphicsDevice& device, HWND hwnd, const CaptureOptions& options)
    : device_(device), hwnd_(hwnd), options_(options) {
    item_ = CreateItemForWindow(hwnd);
    surfaceSize_ = item_.Size();

    framePool_ = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
        device_.winrtDevice, winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        /*numberOfBuffers=*/2, surfaceSize_);

    session_ = framePool_.CreateCaptureSession(item_);

    if (ApiPresent(kCaptureSessionClass, L"IsCursorCaptureEnabled")) {
        session_.IsCursorCaptureEnabled(options_.captureCursor);
    }
    if (ApiPresent(kCaptureSessionClass, L"IsBorderRequired")) {
        // Best effort: some policy configurations refuse to drop the outline.
        try {
            session_.IsBorderRequired(options_.drawBorder);
        } catch (const winrt::hresult_error&) {
        }
    }
    if (options_.maxFramesPerSecond > 0 &&
        ApiPresent(kCaptureSessionClass, L"MinUpdateInterval")) {
        const auto interval = std::chrono::duration_cast<winrt::TimeSpan>(
            std::chrono::duration<double>(1.0 / options_.maxFramesPerSecond));
        session_.MinUpdateInterval(interval);
    }
}

WindowCapture::~WindowCapture() {
    Stop();
}

std::wstring WindowCapture::DisplayName() const {
    return item_ ? std::wstring(item_.DisplayName()) : std::wstring();
}

void WindowCapture::Start(FrameCallback onFrame, ClosedCallback onClosed) {
    {
        std::lock_guard lock(mutex_);
        onFrame_ = std::move(onFrame);
        onClosed_ = std::move(onClosed);
    }

    frameArrived_ = framePool_.FrameArrived(
        winrt::auto_revoke, {this, &WindowCapture::OnFrameArrived});

    itemClosed_ = item_.Closed(winrt::auto_revoke,
                               [this](auto&&, auto&&) {
                                   running_ = false;
                                   ClosedCallback callback;
                                   {
                                       std::lock_guard lock(mutex_);
                                       callback = onClosed_;
                                   }
                                   if (callback) callback();
                               });

    running_ = true;
    session_.StartCapture();
}

void WindowCapture::Stop() {
    if (!running_.exchange(false)) return;

    frameArrived_.revoke();
    itemClosed_.revoke();

    if (session_) session_.Close();
    if (framePool_) framePool_.Close();

    std::lock_guard lock(mutex_);
    onFrame_ = nullptr;
    onClosed_ = nullptr;
}

RECT WindowCapture::ComputeCrop(const D3D11_TEXTURE2D_DESC& desc) const {
    const LONG surfaceW = static_cast<LONG>(desc.Width);
    const LONG surfaceH = static_cast<LONG>(desc.Height);
    RECT full{0, 0, surfaceW, surfaceH};

    if (!options_.cropToClientArea) return full;

    RECT client{};
    if (!GetClientRect(hwnd_, &client)) return full;
    const LONG clientW = client.right - client.left;
    const LONG clientH = client.bottom - client.top;
    if (clientW <= 0 || clientH <= 0) return full;

    // A borderless window's client area already fills the surface. Skip the maths
    // rather than risk an off-by-one on the common case for games.
    if (clientW == surfaceW && clientH == surfaceH) return full;

    POINT clientOrigin{0, 0};
    if (!ClientToScreen(hwnd_, &clientOrigin)) return full;

    // The capture surface is aligned to the window's *visible* bounds. That is
    // not GetWindowRect, which on Windows 10+ also covers the invisible resize
    // border (measurably ~11px per side at 150% DPI, which cost us 10 columns of
    // real pixels before this was fixed). DWM reports the visible rect directly.
    RECT window{};
    if (FAILED(DwmGetWindowAttribute(hwnd_, DWMWA_EXTENDED_FRAME_BOUNDS, &window, sizeof(window)))) {
        if (!GetWindowRect(hwnd_, &window)) return full;
    }

    RECT crop{clientOrigin.x - window.left, clientOrigin.y - window.top, 0, 0};
    crop.right = crop.left + clientW;
    crop.bottom = crop.top + clientH;

    // Clamp: a window straddling a DPI boundary can briefly report a client rect
    // that does not fit the surface we were handed.
    crop.left = std::clamp(crop.left, 0L, surfaceW);
    crop.top = std::clamp(crop.top, 0L, surfaceH);
    crop.right = std::clamp(crop.right, crop.left, surfaceW);
    crop.bottom = std::clamp(crop.bottom, crop.top, surfaceH);

    if (crop.right <= crop.left || crop.bottom <= crop.top) return full;
    return crop;
}

void WindowCapture::OnFrameArrived(winrt::Direct3D11CaptureFramePool const& sender,
                                   winrt::IInspectable const&) {
    if (!running_) return;

    const auto frame = sender.TryGetNextFrame();
    if (!frame) return;

    const auto contentSize = frame.ContentSize();
    bool needsRecreate = false;
    {
        std::lock_guard lock(mutex_);
        if (contentSize.Width != surfaceSize_.Width || contentSize.Height != surfaceSize_.Height) {
            surfaceSize_ = contentSize;
            needsRecreate = true;
        }
    }

    const auto texture = GetTextureFromSurface(frame.Surface());
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    FrameCallback callback;
    {
        std::lock_guard lock(mutex_);
        callback = onFrame_;
    }

    if (callback) {
        CapturedFrame out;
        out.texture = texture.get();
        out.crop = ComputeCrop(desc);
        out.cropWidth = out.crop.right - out.crop.left;
        out.cropHeight = out.crop.bottom - out.crop.top;
        out.timestampHns = frame.SystemRelativeTime().count();
        out.frameIndex = frameIndex_.fetch_add(1);
        callback(out);
    }

    if (needsRecreate) {
        // Must happen after the frame is released, hence at the end of the handler.
        framePool_.Recreate(device_.winrtDevice,
                            winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, contentSize);
    }
}

}  // namespace thorstream
