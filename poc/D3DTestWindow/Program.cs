using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.Mathematics;

namespace D3DTestWindow;

/// <summary>
/// A stand-in for a game: a borderless window with a flip-model D3D11 swapchain
/// presenting as fast as it can. Renders an unmistakable pattern so a captured
/// frame is obviously THIS window and not the desktop behind it.
/// </summary>
internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        bool colourBars = args.Contains("--bars");
        args = args.Where(a => !a.StartsWith("--")).ToArray();

        int width = args.Length > 0 && int.TryParse(args[0], out int w) ? w : 1280;
        int height = args.Length > 1 && int.TryParse(args[1], out int h) ? h : 720;
        int seconds = args.Length > 2 && int.TryParse(args[2], out int s) ? s : 30;

        // Without this the process is DPI-virtualised: it renders at the
        // requested size and DWM upscales to the real client area, so the
        // captured frame is a resampled copy rather than our exact pixels.
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        Application.EnableVisualStyles();

        var form = new Form
        {
            Text = "D3D11 FAKE GAME",
            FormBorderStyle = FormBorderStyle.FixedSingle,
            MaximizeBox = false,
            ClientSize = new System.Drawing.Size(width, height),
            StartPosition = FormStartPosition.Manual,
            Location = new System.Drawing.Point(60, 60),
            BackColor = System.Drawing.Color.Black,
        };
        form.Show();

        // ClientSize is in DIPs, so on a scaled display the window's real client
        // area is larger. Building the swapchain from the requested size would
        // leave DWM stretching our output, which quietly ruins any pixel-exact
        // comparison downstream.
        GetClientRect(form.Handle, out RECT client);
        int physicalWidth = client.Right - client.Left;
        int physicalHeight = client.Bottom - client.Top;
        if (physicalWidth > 0 && physicalHeight > 0)
        {
            width = physicalWidth;
            height = physicalHeight;
        }
        Console.WriteLine($"swapchain {width}x{height} (physical client area)");

        var renderer = new Renderer(form.Handle, width, height, colourBars);
        var life = Stopwatch.StartNew();
        long frames = 0;

        Application.Idle += (_, _) =>
        {
            while (!PeekMessage())
            {
                if (life.Elapsed.TotalSeconds > seconds) { form.Close(); return; }
                renderer.RenderFrame(frames++, life.Elapsed.TotalSeconds);
            }
        };

        form.FormClosed += (_, _) =>
        {
            Console.WriteLine($"presented {frames} frames in {life.Elapsed.TotalSeconds:F1}s " +
                              $"({frames / life.Elapsed.TotalSeconds:F0} fps)");
            renderer.Dispose();
        };

        Application.Run(form);
    }

    [DllImport("user32.dll")]
    private static extern bool PeekMessageW(out NativeMessage msg, IntPtr hWnd, uint min, uint max, uint remove);

    [DllImport("user32.dll")]
    private static extern bool GetClientRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    private static extern bool SetProcessDpiAwarenessContext(IntPtr value);

    private static readonly IntPtr DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = new(-4);

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeMessage { public IntPtr hWnd; public uint msg; public IntPtr w, l; public uint time; public System.Drawing.Point pt; }

    private static bool PeekMessage() => PeekMessageW(out _, IntPtr.Zero, 0, 0, 0);
}

internal sealed class Renderer : IDisposable
{
    private readonly ID3D11Device1 _device;
    private readonly ID3D11DeviceContext1 _context;
    private readonly IDXGISwapChain1 _swapChain;
    private readonly ID3D11RenderTargetView _rtv;
    private readonly ID3D11Texture2D _sprite;
    private readonly int _width, _height;

    /// <summary>
    /// Exact RGB values, so a decoded frame can be compared against them
    /// numerically instead of by eye. Primaries plus a greyscale ramp: hue
    /// errors show in the primaries, range errors show at the ramp's ends.
    /// </summary>
    public static readonly (string Name, byte R, byte G, byte B)[] Bars =
    {
        ("white",   255, 255, 255),
        ("yellow",  255, 255,   0),
        ("cyan",      0, 255, 255),
        ("green",     0, 255,   0),
        ("magenta", 255,   0, 255),
        ("red",     255,   0,   0),
        ("blue",      0,   0, 255),
        ("black",     0,   0,   0),
        ("grey75",  191, 191, 191),
        ("grey50",  128, 128, 128),
        ("grey25",   64,  64,  64),
        ("skin",    224, 172, 139),
    };

    private readonly bool _colourBars;

    public Renderer(IntPtr hwnd, int width, int height, bool colourBars = false)
    {
        _width = width;
        _height = height;
        _colourBars = colourBars;

        D3D11.D3D11CreateDevice(
            null, DriverType.Hardware, DeviceCreationFlags.BgraSupport,
            [FeatureLevel.Level_11_1, FeatureLevel.Level_11_0],
            out ID3D11Device device, out ID3D11DeviceContext context).CheckError();

        _device = device.QueryInterface<ID3D11Device1>();
        _context = context.QueryInterface<ID3D11DeviceContext1>();
        device.Dispose();
        context.Dispose();

        using var dxgiDevice = _device.QueryInterface<IDXGIDevice>();
        using var adapter = dxgiDevice.GetAdapter();
        using var factory = adapter.GetParent<IDXGIFactory2>();

        // Flip-model swapchain - what modern games and DXGI-based engines use.
        var desc = new SwapChainDescription1
        {
            Width = (uint)width,
            Height = (uint)height,
            Format = Format.B8G8R8A8_UNorm,
            BufferCount = 2,
            BufferUsage = Usage.RenderTargetOutput,
            SampleDescription = new SampleDescription(1, 0),
            SwapEffect = SwapEffect.FlipDiscard,
            AlphaMode = Vortice.DXGI.AlphaMode.Ignore,
        };

        _swapChain = factory.CreateSwapChainForHwnd(_device, hwnd, desc);
        factory.MakeWindowAssociation(hwnd, WindowAssociationFlags.IgnoreAltEnter);

        using var backBuffer = _swapChain.GetBuffer<ID3D11Texture2D>(0);
        _rtv = _device.CreateRenderTargetView(backBuffer);
        _sprite = CreateSprite();
        if (_colourBars) _bars = CreateColourBars();
    }

    private ID3D11Texture2D _bars;

    private unsafe ID3D11Texture2D CreateColourBars()
    {
        var pixels = new uint[_width * _height];
        int barWidth = Math.Max(_width / Bars.Length, 1);

        for (int y = 0; y < _height; y++)
        {
            for (int x = 0; x < _width; x++)
            {
                var (_, r, g, b) = Bars[Math.Min(x / barWidth, Bars.Length - 1)];
                // BGRA byte order, matching DXGI_FORMAT_B8G8R8A8_UNORM.
                pixels[y * _width + x] = 0xFF000000u | ((uint)r << 16) | ((uint)g << 8) | b;
            }
        }

        var desc = new Texture2DDescription
        {
            Width = (uint)_width,
            Height = (uint)_height,
            MipLevels = 1,
            ArraySize = 1,
            Format = Format.B8G8R8A8_UNorm,
            SampleDescription = new SampleDescription(1, 0),
            Usage = ResourceUsage.Immutable,
            BindFlags = BindFlags.ShaderResource,
        };

        fixed (uint* p = pixels)
        {
            var data = new SubresourceData((IntPtr)p, (uint)(_width * sizeof(uint)));
            return _device.CreateTexture2D(desc, [data]);
        }
    }

    private const int SpriteSize = 192;

    /// <summary>A four-quadrant marker texture: red / green / blue / yellow with a black cross.</summary>
    private unsafe ID3D11Texture2D CreateSprite()
    {
        var pixels = new uint[SpriteSize * SpriteSize];
        for (int py = 0; py < SpriteSize; py++)
        for (int px = 0; px < SpriteSize; px++)
        {
            bool right = px >= SpriteSize / 2, bottom = py >= SpriteSize / 2;
            uint bgra = (right, bottom) switch
            {
                (false, false) => 0xFF0000FFu, // red   (B,G,R,A little-endian = BGRA)
                (true, false) => 0xFF00FF00u, // green
                (false, true) => 0xFFFF0000u, // blue
                (true, true) => 0xFF00FFFFu, // yellow
            };
            bool cross = Math.Abs(px - SpriteSize / 2) < 4 || Math.Abs(py - SpriteSize / 2) < 4;
            pixels[py * SpriteSize + px] = cross ? 0xFF000000u : bgra;
        }

        var desc = new Texture2DDescription
        {
            Width = SpriteSize,
            Height = SpriteSize,
            MipLevels = 1,
            ArraySize = 1,
            Format = Format.B8G8R8A8_UNorm,
            SampleDescription = new SampleDescription(1, 0),
            Usage = ResourceUsage.Immutable,
            BindFlags = BindFlags.ShaderResource,
        };

        fixed (uint* p = pixels)
        {
            var data = new SubresourceData((IntPtr)p, SpriteSize * sizeof(uint));
            return _device.CreateTexture2D(desc, [data]);
        }
    }

    public void RenderFrame(long frame, double t)
    {
        if (_colourBars)
        {
            // Static and exact: any difference after a round trip is the
            // pipeline's doing, not the content's.
            using var target = _swapChain.GetBuffer<ID3D11Texture2D>(0);
            _context.CopyResource(target, _bars);
            _swapChain.Present(1, PresentFlags.None);
            return;
        }

        // Cycling background so every captured frame is visibly different.
        var bg = new Color4(
            (float)(0.5 + 0.5 * Math.Sin(t * 1.7)),
            (float)(0.5 + 0.5 * Math.Sin(t * 2.3 + 2.0)),
            (float)(0.5 + 0.5 * Math.Sin(t * 3.1 + 4.0)),
            1f);
        _context.ClearRenderTargetView(_rtv, bg);

        // A "sprite" bouncing around the client area. Its exact pixel position in a
        // captured frame proves the capture is window-local, not screen-relative.
        int x = (int)((t * 420) % Math.Max(_width - SpriteSize, 1));
        int y = (int)((t * 260) % Math.Max(_height - SpriteSize, 1));

        using var backBuffer = _swapChain.GetBuffer<ID3D11Texture2D>(0);
        _context.CopySubresourceRegion(backBuffer, 0, (uint)x, (uint)y, 0, _sprite, 0);

        _swapChain.Present(0, PresentFlags.None);
    }

    public void Dispose()
    {
        _bars?.Dispose();
        _sprite.Dispose();
        _rtv.Dispose();
        _swapChain.Dispose();
        _context.Dispose();
        _device.Dispose();
    }
}
