#include "popup_overlay.h"

#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace thorstream {
namespace {

// Anything smaller is a tooltip or a shadow, not something worth showing.
constexpr int kMinimumSize = 120;

// A dialog interrupts something; it does not replace it. Anything covering more
// than this much of the game is an application window, not a popup.
constexpr double kMaximumFraction = 0.85;

struct EnumContext {
    std::vector<HWND> windows;
};

BOOL CALLBACK CollectWindows(HWND window, LPARAM param) {
    auto* context = reinterpret_cast<EnumContext*>(param);
    context->windows.push_back(window);
    return TRUE;
}

}  // namespace

// One popup being captured, with its most recent frame kept in a texture we own
// so it can be composited whenever the next game frame arrives.
struct PopupOverlay::Tracked {
    HWND window = nullptr;
    std::unique_ptr<WindowCapture> capture;
    winrt::com_ptr<ID3D11Texture2D> latest;
    RECT source{};
    RECT onScreen{};
    std::atomic<bool> hasContent{false};
    std::mutex frameMutex;
};

PopupOverlay::PopupOverlay(GraphicsDevice& device, HWND gameWindow)
    : device_(device), gameWindow_(gameWindow) {}

PopupOverlay::~PopupOverlay() {
    Stop();
}

void PopupOverlay::Start() {
    running_ = true;
    scanThread_ = std::thread(&PopupOverlay::ScanLoop, this);
}

void PopupOverlay::Stop() {
    if (!running_.exchange(false)) return;
    if (scanThread_.joinable()) scanThread_.join();

    std::lock_guard lock(mutex_);
    tracked_.clear();
}

bool PopupOverlay::Qualifies(HWND window) const {
    if (window == gameWindow_ || !IsWindowVisible(window)) return false;
    if (GetAncestor(window, GA_ROOT) != window) return false;

    // Cloaked windows are present but not shown - UWP keeps many of them around.
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0) {
        return false;
    }

    const LONG style = GetWindowLongW(window, GWL_STYLE);
    const LONG exStyle = GetWindowLongW(window, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;
    if (exStyle & WS_EX_NOACTIVATE) return false;  // overlays that never take focus

    RECT bounds{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) {
        if (!GetWindowRect(window, &bounds)) return false;
    }
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width < kMinimumSize || height < kMinimumSize) return false;

    // Ordinary application windows are not popups, however they are styled. The
    // first version of this checked only style bits and promptly composited the
    // Settings app over the entire frame, hiding the game completely - so the
    // decisive test is size: a dialog is small relative to what it interrupts.
    RECT gameBounds{};
    if (!GetClientRect(gameWindow_, &gameBounds)) return false;
    const int gameWidth = gameBounds.right - gameBounds.left;
    const int gameHeight = gameBounds.bottom - gameBounds.top;
    if (gameWidth <= 0 || gameHeight <= 0) return false;

    if (width > gameWidth * kMaximumFraction || height > gameHeight * kMaximumFraction) {
        return false;
    }

    // Shell surfaces and app frames are never the dialog we are looking for, and
    // some of them are small enough to slip past the size test.
    wchar_t className[128] = {};
    GetClassNameW(window, className, ARRAYSIZE(className));
    for (const wchar_t* excluded : {L"ApplicationFrameWindow", L"Shell_TrayWnd", L"Progman",
                                    L"WorkerW", L"Windows.UI.Core.CoreWindow", L"XamlExplorerHost"}) {
        if (wcscmp(className, excluded) == 0) return false;
    }

    const bool isDialogClass = wcscmp(className, L"#32770") == 0;
    const bool isOwned = GetWindow(window, GW_OWNER) != nullptr;
    const bool isTopmost = (exStyle & WS_EX_TOPMOST) != 0;
    const bool isPopupStyle = (style & WS_POPUP) != 0;

    return isDialogClass || isOwned || isTopmost || isPopupStyle;
}

void PopupOverlay::ScanLoop() {
    while (running_) {
        EnumContext context;
        EnumWindows(CollectWindows, reinterpret_cast<LPARAM>(&context));

        std::vector<HWND> current;
        for (HWND window : context.windows) {
            if (Qualifies(window)) current.push_back(window);
        }

        std::lock_guard lock(mutex_);

        // Drop anything that has gone away or stopped qualifying.
        tracked_.erase(
            std::remove_if(tracked_.begin(), tracked_.end(),
                           [&](const std::unique_ptr<Tracked>& entry) {
                               return std::find(current.begin(), current.end(), entry->window) ==
                                      current.end();
                           }),
            tracked_.end());

        // Start capturing anything new.
        for (HWND window : current) {
            const bool known = std::any_of(
                tracked_.begin(), tracked_.end(),
                [&](const std::unique_ptr<Tracked>& entry) { return entry->window == window; });
            if (known) continue;

            auto entry = std::make_unique<Tracked>();
            entry->window = window;

            CaptureOptions options;
            options.cropToClientArea = false;  // dialogs are mostly chrome
            options.captureCursor = false;
            options.drawBorder = false;

            try {
                entry->capture = std::make_unique<WindowCapture>(device_, window, options);
            } catch (const winrt::hresult_error&) {
                continue;  // some windows simply cannot be captured
            }

            wchar_t title[128] = {};
            GetWindowTextW(window, title, ARRAYSIZE(title));
            wprintf(L"popup: capturing \"%s\"\n", title);

            Tracked* raw = entry.get();
            raw->capture->Start(
                [this, raw](const CapturedFrame& frame) {
                    D3D11_TEXTURE2D_DESC desc{};
                    frame.texture->GetDesc(&desc);

                    std::lock_guard frameLock(raw->frameMutex);

                    // Copy into our own texture: the captured surface is only
                    // valid for the duration of this callback, but compositing
                    // happens later, when the game's next frame arrives.
                    if (!raw->latest) {
                        D3D11_TEXTURE2D_DESC owned = desc;
                        owned.Usage = D3D11_USAGE_DEFAULT;
                        owned.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                        owned.CPUAccessFlags = 0;
                        owned.MiscFlags = 0;
                        if (FAILED(device_.device->CreateTexture2D(&owned, nullptr,
                                                                   raw->latest.put()))) {
                            return;
                        }
                    }

                    D3D11_TEXTURE2D_DESC existing{};
                    raw->latest->GetDesc(&existing);
                    if (existing.Width != desc.Width || existing.Height != desc.Height) {
                        raw->latest = nullptr;  // resized; rebuilt on the next frame
                        return;
                    }

                    device_.context->CopyResource(raw->latest.get(), frame.texture);
                    raw->source = frame.crop;

                    RECT bounds{};
                    if (FAILED(DwmGetWindowAttribute(raw->window, DWMWA_EXTENDED_FRAME_BOUNDS,
                                                     &bounds, sizeof(bounds)))) {
                        GetWindowRect(raw->window, &bounds);
                    }
                    raw->onScreen = bounds;
                    if (!raw->hasContent.exchange(true)) {
                        wprintf(L"popup: first frame %ux%u at %d,%d\n", desc.Width, desc.Height,
                                bounds.left, bounds.top);
                    }
                },
                [] {});

            tracked_.push_back(std::move(entry));
        }

        // Scanning is cheap, but not free; twice a second is fast enough for a
        // dialog to feel like it appeared immediately.
        for (int i = 0; i < 5 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

std::vector<PopupSnapshot> PopupOverlay::Current() {
    std::vector<PopupSnapshot> snapshots;

    std::lock_guard lock(mutex_);
    for (const auto& entry : tracked_) {
        if (!entry->hasContent) continue;

        std::lock_guard frameLock(entry->frameMutex);
        if (!entry->latest) continue;

        PopupSnapshot snapshot;
        snapshot.texture = entry->latest.get();
        snapshot.source = entry->source;
        snapshot.onScreen = entry->onScreen;
        snapshots.push_back(snapshot);
    }
    return snapshots;
}

}  // namespace thorstream
