// WinSock2 must precede Windows.h or we get the winsock 1.1 declarations.
#include <WinSock2.h>
#include <ws2tcpip.h>

#include <Windows.h>
#include <shellscalingapi.h>
#include <winrt/base.h>

#include <fcntl.h>
#include <io.h>
#include <share.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "d3d_device.h"
#include "encoder_nvenc.h"
#include "gamepad_vigem.h"
#include "save_png.h"
#include "session.h"
#include "window_capture.h"
#include "window_list.h"

namespace {

using namespace thorstream;

struct Options {
    std::wstring filter;
    int seconds = 10;
    bool encode = false;
    bool hevc = false;
    int bitrateKbps = 20000;
    int fpsCap = 0;
    bool serve = false;
    int port = protocol::kDefaultControlPort;
    bool hidden = false;
    std::wstring logPath;
};

// Distinct exit codes, because an autostarted host has nowhere to print.
constexpr int kExitLogOpenFailed = 21;
constexpr int kExitCaptureUnsupported = 22;
constexpr int kExitBadOption = 23;

// Only one host may own the capture ports and the virtual pad. Autostart makes a
// double-launch likely (task fires, then the user starts one by hand), and
// "port already in use" is a poor way to find that out.
HANDLE AcquireSingleInstanceLock() {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\thorstream-host-singleton");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return nullptr;
    }
    return mutex;
}

// Redirects output to a file so an autostarted, windowless host is still
// diagnosable. Appends, because the interesting run is rarely the last one.
// Last-resort diagnostics for a host that failed before it had anywhere to log.
// Deliberately raw Win32: the CRT streams are exactly what is suspect here.
void WriteStartupError(const std::wstring& message) {
    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return;
    std::wstring path = exePath;
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return;
    path = path.substr(0, slash + 1) + L"thorstream-startup-error.txt";

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    const std::string utf8 = [&] {
        const int size = WideCharToMultiByte(CP_UTF8, 0, message.data(),
                                             static_cast<int>(message.size()), nullptr, 0, nullptr,
                                             nullptr);
        std::string out(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()),
                            out.data(), size, nullptr, nullptr);
        return out;
    }();

    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
}

bool RedirectOutputToLog(const std::wstring& path) {
    // Create the parent directory first. An autostarted host runs long before
    // anyone thinks to make its log folder, and "path not found" is otherwise
    // indistinguishable from a permissions problem.
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    // _wfsopen rather than freopen: the default sharing mode locks the file for
    // the entire run, so nobody can tail the log of a host that is still up -
    // exactly when you most want to read it.
    FILE* file = _wfsopen(path.c_str(), L"a", _SH_DENYNO);
    if (!file) {
        WriteStartupError(L"could not open the log file [" + path + L"]\nerrno " +
                          std::to_wstring(errno) + L"\n");
        return false;
    }

    // Point the standard descriptors at the log, leaving the CRT's own stdout
    // stream object intact. Grafting FILE structs around instead turned out to
    // be fragile enough to silently produce empty logs.
    const int logFd = _fileno(file);
    if (logFd < 0 || _dup2(logFd, _fileno(stdout)) != 0) {
        WriteStartupError(L"could not redirect stdout to [" + path + L"]\n");
        fclose(file);
        return false;
    }
    _dup2(logFd, _fileno(stderr));
    return true;
}

// So the user knows what address to type into the handheld.
void PrintLocalAddresses(int port) {
    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname)) != 0) return;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &results) != 0) return;

    for (const addrinfo* it = results; it; it = it->ai_next) {
        char address[INET_ADDRSTRLEN] = {};
        const auto* in = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
        inet_ntop(AF_INET, &in->sin_addr, address, sizeof(address));
        wprintf(L"    %hs:%d\n", address, port);
    }
    freeaddrinfo(results);
}

// Signalled by the console control handler so shutdown runs on the main thread.
HANDLE g_shutdownEvent = nullptr;

BOOL WINAPI ConsoleHandler(DWORD signal) {
    switch (signal) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_shutdownEvent) SetEvent(g_shutdownEvent);
            // Give the main thread a moment to unplug the virtual pad. Returning
            // immediately lets Windows kill us first, which strands the pad and
            // leaves games seeing a phantom controller.
            Sleep(1500);
            return TRUE;
        default:
            return FALSE;
    }
}

// Drives the virtual pad directly, with no networking in the way, so a failure
// here points at ViGEm rather than at the protocol.
int RunGamepadSelfTest() {
    std::string error;
    auto gamepad = VirtualGamepad::Create(&error);
    if (!gamepad) {
        wprintf(L"Could not create the virtual pad: %hs\n", error.c_str());
        return 1;
    }
    wprintf(L"Virtual pad attached (XInput slot %d). Driving it directly...\n",
            gamepad->XInputSlot());

    const struct {
        const wchar_t* name;
        uint16_t buttons;
        int16_t leftX;
    } steps[] = {
        {L"A", 0x1000, 0},
        {L"A+B+X+Y", 0xF000, 0},
        {L"left stick right", 0, 32000},
        {L"released", 0, 0},
    };

    int failures = 0;
    for (const auto& step : steps) {
        protocol::GamepadState state{};
        state.buttons = step.buttons;
        state.leftStickX = step.leftX;
        state.sequence = 1;

        std::string submitError;
        const bool ok = gamepad->Submit(state, &submitError);
        wprintf(L"  %-20s -> %hs\n", step.name, ok ? "accepted" : submitError.c_str());
        if (!ok) ++failures;
        Sleep(400);
    }

    wprintf(failures == 0 ? L"\nDriver accepted every update.\n"
                          : L"\n%d update(s) were rejected by the driver.\n",
            failures);
    return failures == 0 ? 0 : 1;
}

int RunServer(const Options& opts) {
    auto device = CreateGraphicsDevice();
    Session session(device);

    // Optional: without ViGEmBus we can still stream video, just not send input.
    std::string gamepadError;
    auto gamepad = VirtualGamepad::Create(&gamepadError);
    if (gamepad) {
        const int slot = gamepad->XInputSlot();
        if (slot >= 0) {
            wprintf(L"Virtual Xbox 360 pad ready (XInput slot %d).\n", slot);
        } else {
            wprintf(L"Virtual Xbox 360 pad attached but the driver has not assigned it an "
                    L"XInput slot - games will not see it. Stale virtual pads from a host that "
                    L"did not shut down cleanly are the usual cause; a reboot clears them.\n");
        }
        // Report the first rejection rather than dropping input silently: a
        // controller that does nothing with no explanation is the worst outcome.
        session.onGamepad = [pad = gamepad.get(), warned = std::make_shared<bool>(false)](
                                const protocol::GamepadState& state) {
            std::string submitError;
            if (!pad->Submit(state, &submitError) && !*warned) {
                *warned = true;
                wprintf(L"virtual pad rejected input: %hs\n", submitError.c_str());
            }
        };
        session.onReleaseInput = [pad = gamepad.get()] { pad->ReleaseAll(); };
    } else {
        wprintf(L"Gamepad input disabled: %hs\n", gamepadError.c_str());
    }

    std::string error;
    if (!session.Serve(static_cast<uint16_t>(opts.port), &error)) {
        wprintf(L"Failed to start the server: %hs\n", error.c_str());
        return 3;
    }

    wprintf(L"Listening for a client on port %d.\n", opts.port);
    wprintf(L"  Point the Thor client at one of these:\n");
    PrintLocalAddresses(opts.port);
    wprintf(L"\nPress Ctrl+C to stop.\n\n");

    // The server runs on its own threads; park here until the user interrupts.
    // Waiting on an event rather than spinning means Ctrl+C unwinds properly and
    // the virtual gamepad gets unplugged.
    WaitForSingleObject(g_shutdownEvent, INFINITE);

    wprintf(L"\nShutting down...\n");
    session.Shutdown();
    gamepad.reset();
    wprintf(L"Stopped cleanly.\n");
    return 0;
}

void PrintWindows(const std::vector<WindowEntry>& windows) {
    wprintf(L"Capturable top-level windows (%zu):\n", windows.size());
    for (size_t i = 0; i < windows.size(); ++i) {
        const auto& w = windows[i];
        std::wstring title = w.title.size() > 58 ? w.title.substr(0, 55) + L"..." : w.title;
        wprintf(L"  [%2zu] %5dx%-5d %-20.20s %s\n", i, w.clientWidth, w.clientHeight,
                w.process.c_str(), title.c_str());
    }
}

void PrintUsage() {
    wprintf(
        L"Usage: thorstream-host [window-filter] [options]\n"
        L"  --seconds N     how long to run (default 10)\n"
        L"  --encode        encode with NVENC to capture.h264 instead of dumping PNGs\n"
        L"  --hevc          use HEVC instead of H.264 (implies --encode)\n"
        L"  --bitrate N     encoder bitrate in kbps (default 20000)\n"
        L"  --fps N         cap capture rate at N fps (default: uncapped)\n"
        L"  --serve         run as a streaming server for the Thor client\n"
        L"  --port N        control port when serving (default 47810)\n"
        L"  --hidden        detach the console window (for autostart)\n"
        L"  --log FILE      append output to FILE instead of the console\n"
        L"  --gamepad-selftest  drive the virtual pad directly, no networking\n"
        L"With no filter, prints a numbered window list and prompts for a choice.\n");
}

int RunCapture(const WindowEntry& target, const Options& opts) {
    auto device = CreateGraphicsDevice();

    CaptureOptions captureOptions;
    captureOptions.cropToClientArea = true;
    captureOptions.captureCursor = false;
    captureOptions.drawBorder = false;
    captureOptions.maxFramesPerSecond = opts.fpsCap;

    WindowCapture capture(device, target.hwnd, captureOptions);
    wprintf(L"Capture surface   : %dx%d\n", capture.SurfaceSize().Width,
            capture.SurfaceSize().Height);
    wprintf(L"Item display name : %s\n\n", capture.DisplayName().c_str());

    const auto outDir = std::filesystem::current_path() / "capture-frames";
    std::filesystem::create_directories(outDir);

    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> encodedBytes{0};
    std::atomic<uint64_t> keyframes{0};
    std::atomic<int> saved{0};
    std::atomic<bool> closed{false};
    std::atomic<int> lastW{0}, lastH{0};

    std::mutex encoderMutex;
    std::unique_ptr<NvencEncoder> encoder;
    std::ofstream bitstreamFile;
    std::string encoderError;

    if (opts.encode) {
        const auto path = outDir / "capture.h264";
        bitstreamFile.open(path, std::ios::binary | std::ios::trunc);
        wprintf(L"Writing bitstream to %s\n\n", path.wstring().c_str());
    }

    const auto start = std::chrono::steady_clock::now();

    capture.Start(
        [&](const CapturedFrame& frame) {
            const uint64_t n = frames.fetch_add(1) + 1;
            lastW = frame.cropWidth;
            lastH = frame.cropHeight;

            if (opts.encode) {
                std::lock_guard lock(encoderMutex);
                if (!encoder && encoderError.empty()) {
                    EncoderSettings settings;
                    settings.width = frame.cropWidth;
                    settings.height = frame.cropHeight;
                    settings.framerate = opts.fpsCap > 0 ? opts.fpsCap : 60;
                    settings.bitrateKbps = opts.bitrateKbps;
                    settings.useHevc = opts.hevc;
                    encoder = NvencEncoder::Create(device.device.get(), device.context.get(),
                                                   settings, &encoderError);
                    if (encoder) {
                        wprintf(L"NVENC ready: %s %dx%d @ %d kbps, sequence header %zu bytes\n",
                                opts.hevc ? L"HEVC" : L"H.264", settings.width, settings.height,
                                settings.bitrateKbps, encoder->SequenceHeader().size());
                    } else {
                        wprintf(L"NVENC unavailable: %hs\n", encoderError.c_str());
                    }
                }
                if (encoder) {
                    encoder->EncodeFrame(frame.texture, frame.crop, frame.timestampHns,
                                         [&](const EncodedPacket& packet) {
                                             encodedBytes.fetch_add(packet.size);
                                             if (packet.isKeyframe) keyframes.fetch_add(1);
                                             bitstreamFile.write(
                                                 reinterpret_cast<const char*>(packet.data),
                                                 static_cast<std::streamsize>(packet.size));
                                         });
                }
                return;
            }

            if ((n == 2 || n == 60 || n == 200) && saved.load() < 3) {
                saved.fetch_add(1);
                wchar_t name[64];
                swprintf(name, ARRAYSIZE(name), L"frame-%04llu.png",
                         static_cast<unsigned long long>(n));
                const auto path = (outDir / name).wstring();
                if (SaveTextureRegionAsPng(device.device.get(), device.context.get(),
                                           frame.texture, frame.crop, path)) {
                    wprintf(L"  saved %s  (crop %d,%d %dx%d)\n", path.c_str(), frame.crop.left,
                            frame.crop.top, frame.cropWidth, frame.cropHeight);
                }
            }
        },
        [&]() {
            wprintf(L"  target window closed.\n");
            closed = true;
        });

    wprintf(L"Running for %d seconds...\n\n", opts.seconds);
    while (!closed) {
        Sleep(1000);
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const uint64_t n = frames.load();
        const double fps = n / (elapsed > 0.001 ? elapsed : 0.001);
        if (opts.encode) {
            const double mbps = encodedBytes.load() * 8.0 / elapsed / 1e6;
            wprintf(L"  t=%4.1fs  frames=%5llu  %5.1f fps  %6.2f Mbps  keyframes=%llu\n", elapsed,
                    static_cast<unsigned long long>(n), fps, mbps,
                    static_cast<unsigned long long>(keyframes.load()));
        } else {
            wprintf(L"  t=%4.1fs  frames=%5llu  %5.1f fps  crop=%dx%d\n", elapsed,
                    static_cast<unsigned long long>(n), fps, lastW.load(), lastH.load());
        }
        if (elapsed >= opts.seconds) break;
    }

    capture.Stop();
    if (bitstreamFile.is_open()) bitstreamFile.close();

    const auto total =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const uint64_t n = frames.load();

    wprintf(L"\n=== RESULT ===\n");
    wprintf(L"Frames captured   : %llu in %.2fs (%.1f fps average)\n",
            static_cast<unsigned long long>(n), total, n / (total > 0.001 ? total : 0.001));
    wprintf(L"Encoded region    : %dx%d  (surface was %dx%d)\n", lastW.load(), lastH.load(),
            capture.SurfaceSize().Width, capture.SurfaceSize().Height);
    if (opts.encode) {
        const uint64_t bytes = encodedBytes.load();
        wprintf(L"Bitstream         : %.2f MB, %.2f Mbps average, %llu keyframes\n",
                bytes / 1e6, bytes * 8.0 / total / 1e6,
                static_cast<unsigned long long>(keyframes.load()));
        if (!encoder) wprintf(L"Encoder           : FAILED (%hs)\n", encoderError.c_str());
    }
    wprintf(L"Output directory  : %s\n", outDir.wstring().c_str());
    return n > 0 ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // A scheduled task can launch us with no console attached at all. The CRT's
    // default invalid-parameter handler responds to writes on a broken stdout by
    // fast-failing with 0xC0000409 - a crash, before any logging exists to
    // explain it. Swallow those instead.
    _set_invalid_parameter_handler(
        [](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {});

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const auto next = [&]() -> int { return (i + 1 < argc) ? _wtoi(argv[++i]) : 0; };
        if (arg == L"--help" || arg == L"-h") { PrintUsage(); return 0; }
        else if (arg == L"--seconds") opts.seconds = next();
        else if (arg == L"--bitrate") opts.bitrateKbps = next();
        else if (arg == L"--fps") opts.fpsCap = next();
        else if (arg == L"--port") opts.port = next();
        else if (arg == L"--serve") opts.serve = true;
        else if (arg == L"--hidden") opts.hidden = true;
        else if (arg == L"--log") { if (i + 1 < argc) opts.logPath = argv[++i]; }
        else if (arg == L"--gamepad-selftest") return RunGamepadSelfTest();
        else if (arg == L"--encode") opts.encode = true;
        else if (arg == L"--hevc") { opts.hevc = true; opts.encode = true; }
        else if (arg.rfind(L"--", 0) == 0) {
            wprintf(L"Unknown option %s\n", arg.c_str());
            return kExitBadOption;
        }
        else if (opts.filter.empty()) opts.filter = arg;
    }
    if (opts.seconds <= 0) opts.seconds = 10;

    // Hide the console window rather than FreeConsole(). Detaching leaves the CRT
    // holding streams with no valid handles, and reattaching them to a file is
    // fragile enough that it silently produced empty logs. Hiding the window is
    // what "run in the background" actually needs, and stdout stays valid.
    if (opts.hidden) {
        if (HWND console = GetConsoleWindow()) ShowWindow(console, SW_HIDE);
    }

    // Redirect before the first byte of output, so an autostarted host records
    // even its startup failures.
    if (!opts.logPath.empty() && !RedirectOutputToLog(opts.logPath)) {
        // Distinct exit code: when this runs unattended there is, by definition,
        // nowhere to print the reason.
        wprintf(L"Could not open the log file %s\n", opts.logPath.c_str());
        return kExitLogOpenFailed;
    }

    // Unbuffered: a server you cannot tell is running is a server you assume is
    // broken. Safe to do now that stdout is known-good.
    setvbuf(stdout, nullptr, _IONBF, 0);

    wprintf(L"=== thorstream host ===\n");
    if (!WindowCapture::IsSupported()) {
        wprintf(L"Windows.Graphics.Capture is not supported on this machine.\n");
        return kExitCaptureUnsupported;
    }

    if (opts.serve) {
        HANDLE instanceLock = AcquireSingleInstanceLock();
        if (!instanceLock) {
            wprintf(L"Another thorstream host is already running; leaving it alone.\n");
            return 0;
        }

        g_shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);

        const int result = RunServer(opts);
        ReleaseMutex(instanceLock);
        CloseHandle(instanceLock);
        return result;
    }

    const auto windows = EnumerateCapturableWindows();
    if (windows.empty()) {
        wprintf(L"No capturable windows found.\n");
        return 1;
    }

    const WindowEntry* target = nullptr;
    if (!opts.filter.empty()) {
        target = FindWindowByFilter(windows, opts.filter);
        if (!target) {
            wprintf(L"No window matched \"%s\".\n\n", opts.filter.c_str());
            PrintWindows(windows);
            return 1;
        }
    } else {
        PrintWindows(windows);
        wprintf(L"\nPick a window index: ");
        wchar_t line[32] = {};
        if (!fgetws(line, ARRAYSIZE(line), stdin)) return 0;
        const int idx = _wtoi(line);
        if (idx < 0 || static_cast<size_t>(idx) >= windows.size()) {
            wprintf(L"Cancelled.\n");
            return 0;
        }
        target = &windows[static_cast<size_t>(idx)];
    }

    wprintf(L"\nTarget: [%s] \"%s\"  hwnd=0x%llX  client=%dx%d\n\n", target->process.c_str(),
            target->title.c_str(), reinterpret_cast<unsigned long long>(target->hwnd),
            target->clientWidth, target->clientHeight);

    return RunCapture(*target, opts);
}
