using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Intrinsics;
using System.Runtime.Intrinsics.X86;
using BenchmarkDotNet.Attributes;

namespace CNET.Llm.Benchmarks;

/// <summary>
/// Prices the fp16 scale handling in the Q8_0 dot product, at the same stride the
/// real kernel uses (one Half per 34-byte block).
///
/// The anatomy benchmark attributed ~72% of the kernel to "per-block fp16 scale
/// work", but that bundles four separate things: the strided load, the
/// Half-to-float conversion, a scalar multiply and a broadcast. Optimizing the
/// conversion only pays if the conversion is the expensive part, so measure it
/// apart from the rest before writing any SIMD.
/// </summary>
[MemoryDiagnoser]
[SimpleJob(warmupCount: 3, iterationCount: 10)]
public unsafe class HalfConvertBenchmarks
{
    private const int Q8_0BlockBytes = 34;
    private nint _buf;
    private int _blocks;

    [Params(128)]
    public int Blocks { get; set; }

    [GlobalSetup]
    public void Setup()
    {
        var rng = new Random(42);
        _blocks = Blocks;
        _buf = (nint)NativeMemory.AlignedAlloc((nuint)(_blocks * Q8_0BlockBytes), 64);
        for (int b = 0; b < _blocks; b++)
            *(Half*)((byte*)_buf + b * Q8_0BlockBytes) = (Half)(rng.NextSingle() * 0.1f);
    }

    [GlobalCleanup]
    public void Cleanup() => NativeMemory.AlignedFree((void*)_buf);

    /// <summary>Strided load only — the floor.</summary>
    [Benchmark(Baseline = true)]
    public int RawLoad()
    {
        int acc = 0;
        for (int b = 0; b < _blocks; b++)
            acc += Unsafe.ReadUnaligned<ushort>((byte*)_buf + b * Q8_0BlockBytes);
        return acc;
    }

    /// <summary>What the kernel does today: load + (float)Half.</summary>
    [Benchmark]
    public float ScalarHalfConvert()
    {
        float acc = 0;
        for (int b = 0; b < _blocks; b++)
            acc += (float)Unsafe.ReadUnaligned<Half>((byte*)_buf + b * Q8_0BlockBytes);
        return acc;
    }

    /// <summary>
    /// Load + convert + the multiply and broadcast that follow it in the kernel,
    /// so the comparison covers the whole per-block scale sequence.
    /// </summary>
    [Benchmark]
    public float ScalarConvertMulBroadcast()
    {
        Vector256<float> acc = Vector256<float>.Zero;
        for (int b = 0; b < _blocks; b++)
        {
            float d = (float)Unsafe.ReadUnaligned<Half>((byte*)_buf + b * Q8_0BlockBytes);
            acc += Vector256.Create(d * 0.5f);
        }
        return acc.GetElement(0);
    }

    /// <summary>
    /// Manual bit-twiddle fp16 to fp32, to reveal whether the built-in conversion
    /// is already a hardware instruction. If the built-in wins comfortably it is
    /// intrinsified and there is nothing to gain by hand.
    /// </summary>
    [Benchmark]
    public float ManualBitTwiddle()
    {
        float acc = 0;
        for (int b = 0; b < _blocks; b++)
        {
            uint h = Unsafe.ReadUnaligned<ushort>((byte*)_buf + b * Q8_0BlockBytes);
            uint sign = (h & 0x8000u) << 16;
            uint magnitude = h & 0x7FFFu;
            uint bits = magnitude == 0 ? sign : sign | ((magnitude << 13) + 0x38000000u);
            acc += BitConverter.UInt32BitsToSingle(bits);
        }
        return acc;
    }
}
