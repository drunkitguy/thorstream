using System.Runtime.InteropServices;
using System.Text;
using Windows.Graphics.Capture;
using Windows.Graphics.DirectX.Direct3D11;
using WinRT;

namespace WindowCaptureProbe;

/// <summary>
/// The minimum native glue needed to (a) enumerate real top-level windows and
/// (b) hand an HWND to Windows.Graphics.Capture so it captures ONLY that window.
/// </summary>
internal static class Interop
{
    // ---------- window enumeration ----------

    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern IntPtr GetShellWindow();

    [DllImport("user32.dll")]
    public static extern IntPtr GetAncestor(IntPtr hWnd, uint flags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder text, int count);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr hWnd, StringBuilder text, int count);

    [DllImport("user32.dll")]
    public static extern int GetWindowLongW(IntPtr hWnd, int index);

    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    [DllImport("user32.dll")]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr value);

    [DllImport("dwmapi.dll")]
    public static extern int DwmGetWindowAttribute(IntPtr hWnd, int attribute, out int value, int size);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    public const uint GA_ROOT = 2;
    public const int GWL_STYLE = -16;
    public const int GWL_EXSTYLE = -20;
    public const int WS_DISABLED = 0x08000000;
    public const int WS_EX_TOOLWINDOW = 0x00000080;
    public const int DWMWA_CLOAKED = 14;
    public static readonly IntPtr DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = new(-4);

    // ---------- D3D11 device creation ----------

    [DllImport("d3d11.dll", ExactSpelling = true)]
    private static extern int D3D11CreateDevice(
        IntPtr adapter, uint driverType, IntPtr software, uint flags,
        IntPtr featureLevels, uint featureLevelCount, uint sdkVersion,
        out IntPtr device, out uint featureLevel, out IntPtr context);

    [DllImport("d3d11.dll", ExactSpelling = true)]
    private static extern int CreateDirect3D11DeviceFromDXGIDevice(IntPtr dxgiDevice, out IntPtr graphicsDevice);

    private const uint D3D_DRIVER_TYPE_HARDWARE = 1;
    private const uint D3D11_CREATE_DEVICE_BGRA_SUPPORT = 0x20;
    private const uint D3D11_SDK_VERSION = 7;
    private static readonly Guid IID_IDXGIDevice = new("54ec77fa-1377-44e6-8c32-88fd5f44c84c");

    /// <summary>Creates a hardware D3D11 device and projects it as a WinRT IDirect3DDevice.</summary>
    public static IDirect3DDevice CreateDirect3DDevice()
    {
        int hr = D3D11CreateDevice(
            IntPtr.Zero, D3D_DRIVER_TYPE_HARDWARE, IntPtr.Zero,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            IntPtr.Zero, 0, D3D11_SDK_VERSION,
            out IntPtr d3dDevice, out _, out IntPtr context);
        Marshal.ThrowExceptionForHR(hr);

        if (context != IntPtr.Zero) Marshal.Release(context);

        try
        {
            Guid dxgiIid = IID_IDXGIDevice;
            hr = Marshal.QueryInterface(d3dDevice, ref dxgiIid, out IntPtr dxgiDevice);
            Marshal.ThrowExceptionForHR(hr);

            try
            {
                hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, out IntPtr inspectable);
                Marshal.ThrowExceptionForHR(hr);

                try
                {
                    return MarshalInterface<IDirect3DDevice>.FromAbi(inspectable);
                }
                finally { Marshal.Release(inspectable); }
            }
            finally { Marshal.Release(dxgiDevice); }
        }
        finally { Marshal.Release(d3dDevice); }
    }

    // ---------- GraphicsCaptureItem from an HWND ----------

    [ComImport]
    [Guid("3628E81B-3CAC-4C60-B7F4-23CE0E0C3356")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IGraphicsCaptureItemInterop
    {
        IntPtr CreateForWindow([In] IntPtr window, [In] ref Guid iid);
        IntPtr CreateForMonitor([In] IntPtr monitor, [In] ref Guid iid);
    }

    [DllImport("combase.dll", CharSet = CharSet.Unicode)]
    private static extern int WindowsCreateString(string sourceString, int length, out IntPtr hstring);

    [DllImport("combase.dll")]
    private static extern int WindowsDeleteString(IntPtr hstring);

    [DllImport("combase.dll")]
    private static extern int RoGetActivationFactory(IntPtr activatableClassId, ref Guid iid, out IntPtr factory);

    private static readonly Guid IID_IGraphicsCaptureItemInterop = new("3628E81B-3CAC-4C60-B7F4-23CE0E0C3356");
    private static readonly Guid IID_IGraphicsCaptureItem = new("79C3F95B-31F7-4EC2-A464-632EF5D30760");

    /// <summary>
    /// This is the whole point of the probe: a capture item scoped to one HWND.
    /// The compositor feeds us that window's frames only - never the desktop.
    /// </summary>
    public static GraphicsCaptureItem CreateItemForWindow(IntPtr hwnd)
    {
        const string className = "Windows.Graphics.Capture.GraphicsCaptureItem";
        Marshal.ThrowExceptionForHR(WindowsCreateString(className, className.Length, out IntPtr hstring));

        try
        {
            Guid interopIid = IID_IGraphicsCaptureItemInterop;
            Marshal.ThrowExceptionForHR(RoGetActivationFactory(hstring, ref interopIid, out IntPtr factoryPtr));

            try
            {
                var interop = (IGraphicsCaptureItemInterop)Marshal.GetObjectForIUnknown(factoryPtr);
                Guid itemIid = IID_IGraphicsCaptureItem;
                IntPtr itemAbi = interop.CreateForWindow(hwnd, ref itemIid);

                try
                {
                    return MarshalInterface<GraphicsCaptureItem>.FromAbi(itemAbi);
                }
                finally { Marshal.Release(itemAbi); }
            }
            finally { Marshal.Release(factoryPtr); }
        }
        finally { WindowsDeleteString(hstring); }
    }
}
