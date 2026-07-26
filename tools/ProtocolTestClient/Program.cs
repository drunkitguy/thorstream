using System.Buffers.Binary;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;

// A deliberately dumb reference client. It exercises the whole protocol against
// the real host so protocol bugs surface here, in 200 lines of C#, rather than
// inside an Android app where they are far harder to see.
namespace ProtocolTestClient;

internal enum MessageType : byte
{
    Hello = 0x01, WindowList = 0x02, Start = 0x03, Started = 0x04,
    Stop = 0x05, RequestIdr = 0x06, Gamepad = 0x07, Ping = 0x08,
    Pong = 0x09, Error = 0x0A,
}

internal record WindowInfo(ulong Id, uint Width, uint Height, string Process, string Title);

internal static class Program
{
    private const int VideoMagic = 0x31565354; // 'TSV1'
    private const int HeaderSize = 24;

    private static async Task<int> Main(string[] args)
    {
        if (args.Contains("--gamepad"))
        {
            var rest = args.Where(a => a != "--gamepad").ToArray();
            return await GamepadTest.RunAsync(
                rest.Length > 0 ? rest[0] : "127.0.0.1",
                rest.Length > 1 ? int.Parse(rest[1]) : 47810);
        }

        string host = args.Length > 0 ? args[0] : "127.0.0.1";
        int port = args.Length > 1 ? int.Parse(args[1]) : 47810;
        string filter = args.Length > 2 ? args[2] : null;
        int seconds = args.Length > 3 ? int.Parse(args[3]) : 8;
        // Optional max encode size, to exercise the host's downscaling path.
        int maxWidth = args.Length > 4 ? int.Parse(args[4]) : 0;
        int maxHeight = args.Length > 5 ? int.Parse(args[5]) : 0;

        using var tcp = new TcpClient();
        await tcp.ConnectAsync(host, port);
        tcp.NoDelay = true;
        var stream = tcp.GetStream();
        Console.WriteLine($"connected to {host}:{port}");

        using var udp = new UdpClient(0);
        int udpPort = ((IPEndPoint)udp.Client.LocalEndPoint).Port;
        udp.Client.ReceiveBufferSize = 8 * 1024 * 1024;
        Console.WriteLine($"listening for video on udp/{udpPort}");

        // HELLO
        var hello = new Writer();
        hello.U16(1);
        hello.Str("protocol-test-client");
        await SendAsync(stream, MessageType.Hello, hello);

        // WINDOW_LIST
        var (type, payload) = await ReadMessageAsync(stream);
        if (type != MessageType.WindowList) { Console.WriteLine($"expected WINDOW_LIST, got {type}"); return 1; }

        var windows = ParseWindowList(payload);
        Console.WriteLine($"\nhost offered {windows.Count} windows:");
        foreach (var (w, i) in windows.Select((w, i) => (w, i)))
            Console.WriteLine($"  [{i,2}] {w.Width,5}x{w.Height,-5} {w.Process,-22} {Truncate(w.Title, 50)}");

        var target = filter is null
            ? windows.FirstOrDefault()
            : windows.FirstOrDefault(w => w.Title.Contains(filter, StringComparison.OrdinalIgnoreCase)
                                       || w.Process.Contains(filter, StringComparison.OrdinalIgnoreCase));
        if (target is null) { Console.WriteLine($"\nno window matched \"{filter}\""); return 1; }
        Console.WriteLine($"\nrequesting: {target.Process} \"{Truncate(target.Title, 40)}\" ({target.Width}x{target.Height})");

        // START
        var start = new Writer();
        start.U64(target.Id);
        start.U32((uint)maxWidth);   // 0 = unconstrained
        start.U32((uint)maxHeight);
        start.U32(60);           // fps
        start.U32(30000);        // kbps
        start.U8(0);             // H.264
        start.U16((ushort)udpPort);
        await SendAsync(stream, MessageType.Start, start);

        (type, payload) = await ReadMessageAsync(stream);
        if (type == MessageType.Error)
        {
            var r = new Reader(payload);
            Console.WriteLine($"host refused: {r.Str()}");
            return 1;
        }
        if (type != MessageType.Started) { Console.WriteLine($"expected STARTED, got {type}"); return 1; }

        var reader = new Reader(payload);
        uint width = reader.U32(), height = reader.U32();
        byte codec = reader.U8();
        int headerLength = reader.U16();
        byte[] sequenceHeader = reader.Bytes(headerLength);
        Console.WriteLine($"STARTED: {width}x{height} codec={(codec == 0 ? "H.264" : "HEVC")} sequenceHeader={sequenceHeader.Length} bytes\n");

        // Collect video.
        var assembler = new FrameAssembler();
        string outPath = Path.Combine(AppContext.BaseDirectory, "received.h264");
        await using var file = File.Create(outPath);
        await file.WriteAsync(sequenceHeader);

        var sw = Stopwatch.StartNew();
        long datagrams = 0, bytes = 0;
        int completed = 0, dropped = 0, keyframes = 0;
        var cts = new CancellationTokenSource(TimeSpan.FromSeconds(seconds));

        try
        {
            while (!cts.IsCancellationRequested)
            {
                var result = await udp.ReceiveAsync(cts.Token);
                datagrams++;
                bytes += result.Buffer.Length;

                if (assembler.Add(result.Buffer, out byte[] frame, out bool keyframe, out int lost))
                {
                    completed++;
                    if (keyframe) keyframes++;
                    await file.WriteAsync(frame);
                }
                dropped += lost;
            }
        }
        catch (OperationCanceledException) { }

        sw.Stop();
        await SendAsync(stream, MessageType.Stop, new Writer());

        Console.WriteLine("=== RESULT ===");
        Console.WriteLine($"datagrams received : {datagrams}");
        Console.WriteLine($"frames reassembled : {completed} ({completed / sw.Elapsed.TotalSeconds:F1} fps), {keyframes} keyframes");
        Console.WriteLine($"frames dropped     : {dropped}");
        Console.WriteLine($"throughput         : {bytes * 8.0 / sw.Elapsed.TotalSeconds / 1e6:F2} Mbps");
        Console.WriteLine($"written to         : {outPath}");
        return completed > 0 ? 0 : 1;
    }

    private static string Truncate(string s, int max) => s.Length <= max ? s : s[..(max - 3)] + "...";

    private static List<WindowInfo> ParseWindowList(byte[] payload)
    {
        var reader = new Reader(payload);
        int count = reader.U16();
        var result = new List<WindowInfo>(count);
        for (int i = 0; i < count; i++)
        {
            ulong id = reader.U64();
            uint w = reader.U32(), h = reader.U32();
            result.Add(new WindowInfo(id, w, h, reader.Str(), reader.Str()));
        }
        return result;
    }

    private static async Task SendAsync(NetworkStream stream, MessageType type, Writer payload)
    {
        var body = payload.ToArray();
        var frame = new byte[4 + 1 + body.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(frame, (uint)(body.Length + 1));
        frame[4] = (byte)type;
        body.CopyTo(frame, 5);
        await stream.WriteAsync(frame);
    }

    private static async Task<(MessageType, byte[])> ReadMessageAsync(NetworkStream stream)
    {
        var lengthBytes = new byte[4];
        await stream.ReadExactlyAsync(lengthBytes);
        uint length = BinaryPrimitives.ReadUInt32LittleEndian(lengthBytes);

        var payload = new byte[length];
        await stream.ReadExactlyAsync(payload);
        return ((MessageType)payload[0], payload[1..]);
    }

    /// <summary>Reassembles fragmented frames, discarding anything stale.</summary>
    private sealed class FrameAssembler
    {
        private readonly Dictionary<uint, byte[][]> pending = new();
        private readonly Dictionary<uint, bool> keyframes = new();
        private uint newestCompleted;

        public bool Add(byte[] datagram, out byte[] frame, out bool keyframe, out int lost)
        {
            frame = null; keyframe = false; lost = 0;
            if (datagram.Length < HeaderSize) return false;

            var span = datagram.AsSpan();
            if (BinaryPrimitives.ReadInt32LittleEndian(span) != VideoMagic) return false;

            uint frameNumber = BinaryPrimitives.ReadUInt32LittleEndian(span[4..]);
            int index = BinaryPrimitives.ReadUInt16LittleEndian(span[8..]);
            int count = BinaryPrimitives.ReadUInt16LittleEndian(span[10..]);
            byte flags = span[20];
            int payloadSize = BinaryPrimitives.ReadUInt16LittleEndian(span[22..]);

            if (count == 0 || index >= count || HeaderSize + payloadSize > datagram.Length) return false;
            if (frameNumber < newestCompleted) return false; // already superseded

            if (!pending.TryGetValue(frameNumber, out var fragments))
            {
                fragments = new byte[count][];
                pending[frameNumber] = fragments;
                keyframes[frameNumber] = (flags & 0x01) != 0;
            }
            fragments[index] = datagram[HeaderSize..(HeaderSize + payloadSize)];

            if (fragments.Any(f => f is null)) return false;

            frame = fragments.SelectMany(f => f).ToArray();
            keyframe = keyframes[frameNumber];
            newestCompleted = frameNumber;

            // Anything older than the frame we just completed is dead weight.
            foreach (var stale in pending.Keys.Where(k => k <= frameNumber).ToList())
            {
                if (stale != frameNumber) lost++;
                pending.Remove(stale);
                keyframes.Remove(stale);
            }
            return true;
        }
    }

    private sealed class Writer
    {
        private readonly List<byte> bytes = new();
        public void U8(byte v) => bytes.Add(v);
        public void U16(ushort v) { var b = new byte[2]; BinaryPrimitives.WriteUInt16LittleEndian(b, v); bytes.AddRange(b); }
        public void U32(uint v) { var b = new byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(b, v); bytes.AddRange(b); }
        public void U64(ulong v) { var b = new byte[8]; BinaryPrimitives.WriteUInt64LittleEndian(b, v); bytes.AddRange(b); }
        public void Str(string s) { var b = Encoding.UTF8.GetBytes(s); U16((ushort)b.Length); bytes.AddRange(b); }
        public byte[] ToArray() => bytes.ToArray();
    }

    private sealed class Reader(byte[] data)
    {
        private int offset;
        public byte U8() => data[offset++];
        public ushort U16() { var v = BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(offset)); offset += 2; return v; }
        public uint U32() { var v = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(offset)); offset += 4; return v; }
        public ulong U64() { var v = BinaryPrimitives.ReadUInt64LittleEndian(data.AsSpan(offset)); offset += 8; return v; }
        public byte[] Bytes(int n) { var v = data[offset..(offset + n)]; offset += n; return v; }
        public string Str() { int n = U16(); var s = Encoding.UTF8.GetString(data, offset, n); offset += n; return s; }
    }
}
