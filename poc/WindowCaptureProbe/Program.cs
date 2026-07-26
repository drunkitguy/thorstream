using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using Windows.Graphics.Capture;
using Windows.Graphics.DirectX;
using Windows.Graphics.Imaging;
using Windows.Storage.Streams;

namespace WindowCaptureProbe;

internal record WindowInfo(IntPtr Hwnd, string Title, string ClassName, string Process, int Width, int Height);

internal static class Program
{
    private static int Main(string[] args)
    {
        Interop.SetProcessDpiAwarenessContext(Interop.DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        Console.WriteLine("=== Window-only capture probe ===");
        Console.WriteLine($"OS build            : {Environment.OSVersion.Version}");

        bool supported;
        try { supported = GraphicsCaptureSession.IsSupported(); }
        catch (Exception ex) { Console.WriteLine($"IsSupported() threw: {ex.Message}"); return 2; }

        Console.WriteLine($"WGC supported       : {supported}");
        if (!supported) { Console.WriteLine("Windows.Graphics.Capture is unavailable on this machine."); return 2; }

        Console.WriteLine($"Borderless capture  : {IsApiPresent("IsBorderRequired")}  (Win11 - hides the yellow capture outline)");
        Console.WriteLine($"Cursor toggle       : {IsApiPresent("IsCursorCaptureEnabled")}");
        Console.WriteLine($"MinUpdateInterval   : {IsApiPresent("MinUpdateInterval")}  (Win11 22H2+ - lets us cap capture fps)");
        Console.WriteLine($"Dirty-region info   : {IsFramePropertyPresent("DirtyRegions")}");
        Console.WriteLine();

        var windows = EnumerateWindows();
        if (windows.Count == 0) { Console.WriteLine("No capturable windows found."); return 1; }

        WindowInfo target;
        string filter = args.FirstOrDefault(a => !a.StartsWith('-'));

        if (filter is not null)
        {
            target = windows.FirstOrDefault(w => w.Title.Contains(filter, StringComparison.OrdinalIgnoreCase)
                                              || w.Process.Contains(filter, StringComparison.OrdinalIgnoreCase));
            if (target is null) { Console.WriteLine($"No window matched \"{filter}\"."); ListWindows(windows); return 1; }
        }
        else
        {
            ListWindows(windows);
            Console.Write("\nPick a window index to capture (Enter = cancel): ");
            string line = Console.ReadLine();
            if (!int.TryParse(line, out int idx) || idx < 0 || idx >= windows.Count) { Console.WriteLine("Cancelled."); return 0; }
            target = windows[idx];
        }

        Console.WriteLine($"\nTarget: [{target.Process}] \"{target.Title}\"  hwnd=0x{target.Hwnd:X}  client={target.Width}x{target.Height}\n");
        return RunCapture(target);
    }

    private static int RunCapture(WindowInfo target)
    {
        var device = Interop.CreateDirect3DDevice();
        GraphicsCaptureItem item;
        try
        {
            item = Interop.CreateItemForWindow(target.Hwnd);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"CreateForWindow failed: 0x{ex.HResult:X8} {ex.Message}");
            return 3;
        }

        Console.WriteLine($"Capture item created. Reported size: {item.Size.Width}x{item.Size.Height}");
        Console.WriteLine($"Item display name  : {item.DisplayName}");

        var pool = Direct3D11CaptureFramePool.CreateFreeThreaded(
            device, DirectXPixelFormat.B8G8R8A8UIntNormalized, 2, item.Size);
        var session = pool.CreateCaptureSession(item);

        TrySet(session, "IsCursorCaptureEnabled", false);
        bool borderless = TrySet(session, "IsBorderRequired", false);

        string outDir = Path.Combine(AppContext.BaseDirectory, "probe-frames");
        Directory.CreateDirectory(outDir);

        int frames = 0;
        int saved = 0;
        var sw = Stopwatch.StartNew();
        var done = new ManualResetEventSlim(false);
        var lastSize = item.Size;

        pool.FrameArrived += (s, _) =>
        {
            using var frame = s.TryGetNextFrame();
            if (frame is null) return;

            int n = Interlocked.Increment(ref frames);
            lastSize = frame.ContentSize;

            // Save a few frames as proof of what actually lands in the buffer.
            if (n is 2 or 60 or 200 && saved < 3)
            {
                saved++;
                try
                {
                    string path = Path.Combine(outDir, $"frame-{n:D4}.png");
                    SaveSurface(frame.Surface, path).GetAwaiter().GetResult();
                    Console.WriteLine($"  saved {path}");
                }
                catch (Exception ex) { Console.WriteLine($"  save failed: {ex.Message}"); }
            }

            if (sw.Elapsed.TotalSeconds >= 10) done.Set();
        };

        item.Closed += (_, _) => { Console.WriteLine("  target window closed."); done.Set(); };

        session.StartCapture();
        Console.WriteLine("Capturing for 10 seconds - move/resize the window, drag another window over it, alt-tab away.\n");

        var reporter = new Timer(_ =>
        {
            Console.WriteLine($"  t={sw.Elapsed.TotalSeconds,4:F1}s  frames={Volatile.Read(ref frames),5}  " +
                              $"avg={frames / Math.Max(sw.Elapsed.TotalSeconds, 0.001),6:F1} fps  content={lastSize.Width}x{lastSize.Height}");
        }, null, 1000, 1000);

        done.Wait(TimeSpan.FromSeconds(15));
        reporter.Dispose();
        sw.Stop();

        session.Dispose();
        pool.Dispose();

        Console.WriteLine();
        Console.WriteLine("=== RESULT ===");
        Console.WriteLine($"Frames delivered   : {frames} in {sw.Elapsed.TotalSeconds:F2}s  ({frames / sw.Elapsed.TotalSeconds:F1} fps average)");
        Console.WriteLine($"Final content size : {lastSize.Width}x{lastSize.Height}");
        Console.WriteLine($"Borderless allowed : {borderless}");
        Console.WriteLine($"PNGs written to    : {outDir}");
        Console.WriteLine();
        Console.WriteLine(frames > 0
            ? "VERDICT: per-window capture WORKS. Open the PNGs - they contain only the target window."
            : "VERDICT: no frames arrived. The window may be minimised, cloaked, or using exclusive fullscreen.");
        return frames > 0 ? 0 : 4;
    }

    private static async Task SaveSurface(Windows.Graphics.DirectX.Direct3D11.IDirect3DSurface surface, string path)
    {
        using var bitmap = await SoftwareBitmap.CreateCopyFromSurfaceAsync(surface, BitmapAlphaMode.Premultiplied);
        using var ras = new InMemoryRandomAccessStream();
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, ras);
        encoder.SetSoftwareBitmap(SoftwareBitmap.Convert(bitmap, BitmapPixelFormat.Bgra8, BitmapAlphaMode.Straight));
        await encoder.FlushAsync();

        ras.Seek(0);
        using var source = ras.AsStreamForRead();
        using var file = File.Create(path);
        await source.CopyToAsync(file);
    }

    private static List<WindowInfo> EnumerateWindows()
    {
        var result = new List<WindowInfo>();
        IntPtr shell = Interop.GetShellWindow();
        var titleBuf = new StringBuilder(512);
        var classBuf = new StringBuilder(256);

        Interop.EnumWindows((hwnd, _) =>
        {
            if (hwnd == shell) return true;
            if (!Interop.IsWindowVisible(hwnd)) return true;
            if (Interop.GetAncestor(hwnd, Interop.GA_ROOT) != hwnd) return true;

            int style = Interop.GetWindowLongW(hwnd, Interop.GWL_STYLE);
            if ((style & Interop.WS_DISABLED) != 0) return true;

            int exStyle = Interop.GetWindowLongW(hwnd, Interop.GWL_EXSTYLE);
            if ((exStyle & Interop.WS_EX_TOOLWINDOW) != 0) return true;

            // UWP/packaged apps park hidden windows on the desktop; DWM flags them cloaked.
            if (Interop.DwmGetWindowAttribute(hwnd, Interop.DWMWA_CLOAKED, out int cloaked, sizeof(int)) == 0 && cloaked != 0)
                return true;

            titleBuf.Clear();
            if (Interop.GetWindowTextW(hwnd, titleBuf, titleBuf.Capacity) == 0) return true;

            classBuf.Clear();
            Interop.GetClassNameW(hwnd, classBuf, classBuf.Capacity);

            Interop.GetClientRect(hwnd, out var rc);
            int w = rc.Right - rc.Left, h = rc.Bottom - rc.Top;
            if (w < 64 || h < 64) return true;

            Interop.GetWindowThreadProcessId(hwnd, out uint pid);
            string proc = "?";
            try { proc = Process.GetProcessById((int)pid).ProcessName; } catch { }

            result.Add(new WindowInfo(hwnd, titleBuf.ToString(), classBuf.ToString(), proc, w, h));
            return true;
        }, IntPtr.Zero);

        return result;
    }

    private static void ListWindows(List<WindowInfo> windows)
    {
        Console.WriteLine($"Capturable top-level windows ({windows.Count}):");
        for (int i = 0; i < windows.Count; i++)
        {
            var w = windows[i];
            string title = w.Title.Length > 58 ? w.Title[..55] + "..." : w.Title;
            Console.WriteLine($"  [{i,2}] {w.Width,5}x{w.Height,-5} {w.Process,-20} {title}");
        }
    }

    private static bool TrySet(GraphicsCaptureSession session, string property, bool value)
    {
        var prop = typeof(GraphicsCaptureSession).GetProperty(property);
        if (prop is null) return false;
        try { prop.SetValue(session, value); return true; }
        catch { return false; }
    }

    private static bool IsApiPresent(string property) =>
        typeof(GraphicsCaptureSession).GetProperty(property) is not null;

    private static bool IsFramePropertyPresent(string property) =>
        typeof(Direct3D11CaptureFrame).GetProperty(property) is not null;
}
