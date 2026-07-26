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

        public long packetDelta(XInputState earlier) => (long)dwPacketNumber - earlier.dwPacketNumber;
    }

    [DllImport("xinput1_4.dll")]
    private static extern int XInputGetState(int userIndex, ref XInputState state);

    private const int ERROR_SUCCESS = 0;

    private record Step(string Name, ushort Buttons, byte LeftTrigger, byte RightTrigger,
                        short LeftX, short LeftY, short RightX, short RightY);

    /// Holds a button combination down so an external tool can observe the pad.
    public static async Task<int> HoldAsync(string host, int port, int seconds)
    {
        using var tcp = new TcpClient();
        await tcp.ConnectAsync(host, port);
        tcp.NoDelay = true;
        var stream = tcp.GetStream();

        var hello = new Writer();
        hello.U16(1);
        hello.Str("gamepad-hold");
        await SendAsync(stream, MessageType.Hello, hello);
        await Task.Delay(300);

        const ushort combo = 0x1000 | 0x2000 | 0x4000 | 0x8000;
        Console.WriteLine($"holding A+B+X+Y (0x{combo:X4}) for {seconds}s...");

        var deadline = DateTime.UtcNow.AddSeconds(seconds);
        while (DateTime.UtcNow < deadline)
        {
            await SendButtonsAsync(stream, combo);
            await Task.Delay(100);
        }

        await SendButtonsAsync(stream, 0);
        Console.WriteLine("released.");
        return 0;
    }

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

        int slot = await FindOurPadAsync(stream);
        if (slot < 0)
        {
            Console.WriteLine("\nCould not find a virtual pad responding to us.");
            Console.WriteLine("Is the host running with --serve, and did it report");
            Console.WriteLine("'Virtual Xbox 360 pad ready'?");
            return 1;
        }
        Console.WriteLine($"our pad is XInput slot {slot}\n");

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

        int passed = 0;

        foreach (var step in steps)
        {
            var payload = new Writer();
            payload.U16(step.Buttons);
            payload.U8(step.LeftTrigger);
            payload.U8(step.RightTrigger);
            payload.U16((ushort)step.LeftX);
            payload.U16((ushort)step.LeftY);
            payload.U16((ushort)step.RightX);
            payload.U16((ushort)step.RightY);
            payload.U32(++sequence);
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

    /// <summary>
    /// Identifies which XInput slot is ours by pressing a distinctive combination
    /// and seeing which slot reacts. Taking the first connected slot is wrong: a
    /// real controller, or a pad stranded by a host that did not shut down
    /// cleanly, can sit in a lower slot and report nothing forever.
    /// </summary>
    private static async Task<int> FindOurPadAsync(NetworkStream stream)
    {
        const ushort probe = 0x1000 | 0x2000 | 0x4000 | 0x8000; // A+B+X+Y at once

        var before = new XInputState[4];
        for (int i = 0; i < 4; i++) XInputGetState(i, ref before[i]);

        await SendButtonsAsync(stream, probe);
        await Task.Delay(250);

        int found = -1;
        for (int i = 0; i < 4; i++)
        {
            var now = new XInputState();
            int result = XInputGetState(i, ref now);
            Console.WriteLine($"    probe slot {i}: rc={result} packet={now.packetDelta(before[i])} " +
                              $"buttons=0x{now.Gamepad.wButtons:X4} (want 0x{probe:X4})");
            if (result != ERROR_SUCCESS) continue;
            if (now.Gamepad.wButtons == probe) { found = i; break; }
        }

        await SendButtonsAsync(stream, 0); // release
        await Task.Delay(150);
        return found;
    }

    // One monotonic counter for the whole run: the host drops input that appears
    // to go backwards, so the probe must not restart the sequence.
    private static uint sequence;

    private static async Task SendButtonsAsync(NetworkStream stream, ushort buttons)
    {
        var payload = new Writer();
        payload.U16(buttons);
        payload.U8(0); payload.U8(0);
        payload.U16(0); payload.U16(0); payload.U16(0); payload.U16(0);
        payload.U32(++sequence);
        await SendAsync(stream, MessageType.Gamepad, payload);
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
