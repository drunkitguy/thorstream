using System.Net.Sockets;
using System.Runtime.InteropServices;

namespace ProtocolTestClient;

/// <summary>
/// Sends gamepad state to the host and reads it back through XInput, which is
/// exactly how a game would see it. Proves the virtual pad is real rather than
/// just that no error was returned.
/// </summary>
internal static class GamepadTest
{
    [StructLayout(LayoutKind.Sequential)]
    private struct XInputGamepad
    {
        public ushort wButtons;
        public byte bLeftTrigger;
        public byte bRightTrigger;
        public short sThumbLX, sThumbLY, sThumbRX, sThumbRY;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct XInputState
    {
        public uint dwPacketNumber;
        public XInputGamepad Gamepad;
    }

    [DllImport("xinput1_4.dll")]
    private static extern int XInputGetState(int userIndex, ref XInputState state);

    private const int ERROR_SUCCESS = 0;

    private record Step(string Name, ushort Buttons, byte LeftTrigger, byte RightTrigger,
                        short LeftX, short LeftY, short RightX, short RightY);

    public static async Task<int> RunAsync(string host, int port)
    {
        using var tcp = new TcpClient();
        await tcp.ConnectAsync(host, port);
        tcp.NoDelay = true;
        var stream = tcp.GetStream();
        Console.WriteLine($"connected to {host}:{port}");

        var hello = new Writer();
        hello.U16(1);
        hello.Str("gamepad-test");
        await SendAsync(stream, MessageType.Hello, hello);
        await Task.Delay(500);

        int slot = FindVirtualPad();
        if (slot < 0)
        {
            Console.WriteLine("\nNo XInput controller found. Is the host running with --serve,");
            Console.WriteLine("and did it report 'Virtual Xbox 360 pad ready'?");
            return 1;
        }
        Console.WriteLine($"found an XInput pad in slot {slot}\n");

        var steps = new[]
        {
            new Step("A pressed",            0x1000, 0,   0,   0,      0,     0,     0),
            new Step("B + right shoulder",   0x2200, 0,   0,   0,      0,     0,     0),
            new Step("left stick full right",0,      0,   0,   32000,  0,     0,     0),
            new Step("left stick full up",   0,      0,   0,   0,      32000, 0,     0),
            new Step("both triggers",        0,      200, 255, 0,      0,     0,     0),
            new Step("right stick down-left",0,      0,   0,   0,      0,   -30000, -30000),
            new Step("d-pad up + start",     0x0011, 0,   0,   0,      0,     0,     0),
            new Step("released",             0,      0,   0,   0,      0,     0,     0),
        };

        uint sequence = 0;
        int passed = 0;

        foreach (var step in steps)
        {
            sequence++;
            var payload = new Writer();
            payload.U16(step.Buttons);
            payload.U8(step.LeftTrigger);
            payload.U8(step.RightTrigger);
            payload.U16((ushort)step.LeftX);
            payload.U16((ushort)step.LeftY);
            payload.U16((ushort)step.RightX);
            payload.U16((ushort)step.RightY);
            payload.U32(sequence);
            await SendAsync(stream, MessageType.Gamepad, payload);

            await Task.Delay(120);

            var state = new XInputState();
            XInputGetState(slot, ref state);
            var g = state.Gamepad;

            bool ok = g.wButtons == step.Buttons
                   && g.bLeftTrigger == step.LeftTrigger
                   && g.bRightTrigger == step.RightTrigger
                   && Close(g.sThumbLX, step.LeftX) && Close(g.sThumbLY, step.LeftY)
                   && Close(g.sThumbRX, step.RightX) && Close(g.sThumbRY, step.RightY);

            if (ok) passed++;
            Console.WriteLine($"  {(ok ? "PASS" : "FAIL")}  {step.Name,-26} " +
                              $"buttons=0x{g.wButtons:X4} lt={g.bLeftTrigger} rt={g.bRightTrigger} " +
                              $"L=({g.sThumbLX},{g.sThumbLY}) R=({g.sThumbRX},{g.sThumbRY})");
        }

        Console.WriteLine($"\n{passed}/{steps.Length} steps matched what XInput reported back.");
        return passed == steps.Length ? 0 : 1;
    }

    // XInput can quantise thumbstick values slightly on the way through.
    private static bool Close(short actual, short expected) => Math.Abs(actual - expected) <= 256;

    private static int FindVirtualPad()
    {
        for (int i = 0; i < 4; i++)
        {
            var state = new XInputState();
            if (XInputGetState(i, ref state) == ERROR_SUCCESS) return i;
        }
        return -1;
    }

    private static async Task SendAsync(NetworkStream stream, MessageType type, Writer payload)
    {
        var body = payload.ToArray();
        var frame = new byte[4 + 1 + body.Length];
        System.Buffers.Binary.BinaryPrimitives.WriteUInt32LittleEndian(frame, (uint)(body.Length + 1));
        frame[4] = (byte)type;
        body.CopyTo(frame, 5);
        await stream.WriteAsync(frame);
        await stream.FlushAsync();
    }

    internal sealed class Writer
    {
        private readonly List<byte> bytes = new();
        public void U8(int v) => bytes.Add((byte)v);
        public void U16(int v) { bytes.Add((byte)(v & 0xFF)); bytes.Add((byte)((v >> 8) & 0xFF)); }
        public void U32(uint v) { for (int s = 0; s < 32; s += 8) bytes.Add((byte)(v >> s)); }
        public void U64(ulong v) { for (int s = 0; s < 64; s += 8) bytes.Add((byte)(v >> s)); }
        public void Str(string s)
        {
            var b = System.Text.Encoding.UTF8.GetBytes(s);
            U16(b.Length);
            bytes.AddRange(b);
        }
        public byte[] ToArray() => bytes.ToArray();
    }
}
