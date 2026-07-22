using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Intrinsics;
using System.Runtime.Intrinsics.X86;
using BenchmarkDotNet.Attributes;
using CNET.Llm.Cpu.Kernels;

namespace CNET.Llm.Benchmarks;

/// <summary>
/// Decomposes the Q8_0 dot product to find what actually costs the ~8
/// cycles/block the full kernel spends.
///
/// Each benchmark strips one more layer off the real kernel. Only
/// <see cref="Full"/> computes a correct result — the rest deliberately return
/// wrong values and exist purely to price the pieces:
///
///   Full        loads + sign + dot + cvt + per-block fp16 scale + fma
///   ConstScale  as Full, but the per-block Half scales are replaced by a
///               constant — prices the two fp16 loads, the scalar multiply and
///               the broadcast
///   IntAccum    loads + sign + dot, accumulating in int32 — drops cvt and fma
///   DotOnly     loads + dot, no sign normalisation
///   LoadsOnly   loads only — the floor set by feeding the unit
/// </summary>
[MemoryDiagnoser]
[SimpleJob(warmupCount: 3, iterationCount: 10)]
public unsafe class Q8DotAnatomyBenchmarks
{
    private const int Q8_0BlockBytes = 34;
    private const int Q8_0GroupSize = 32;

    private nint _w;
    private nint _x;
    private nint _scalesW;
    private nint _scalesX;
    private int _blocks;

    [Params(4096)]
    public int K { get; set; }

    [GlobalSetup]
    public void Setup()
    {
        var rng = new Random(42);
        _blocks = K / Q8_0GroupSize;
        int rowBytes = _blocks * Q8_0BlockBytes;
        _w = (nint)NativeMemory.AlignedAlloc((nuint)rowBytes, 64);
        _x = (nint)NativeMemory.AlignedAlloc((nuint)rowBytes, 64);
        foreach (nint buf in new[] { _w, _x })
            for (int b = 0; b < _blocks; b++)
            {
                byte* block = (byte*)buf + b * Q8_0BlockBytes;
                *(Half*)block = (Half)(rng.NextSingle() * 0.1f);
                for (int i = 0; i < Q8_0GroupSize; i++)
                    ((sbyte*)(block + 2))[i] = (sbyte)rng.Next(-127, 128);
            }

        _scalesW = (nint)NativeMemory.AlignedAlloc((nuint)(_blocks * sizeof(float)), 64);
        _scalesX = (nint)NativeMemory.AlignedAlloc((nuint)(_blocks * sizeof(float)), 64);
        for (int b = 0; b < _blocks; b++)
        {
            ((float*)_scalesW)[b] = (float)*(Half*)((byte*)_w + b * Q8_0BlockBytes);
            ((float*)_scalesX)[b] = (float)*(Half*)((byte*)_x + b * Q8_0BlockBytes);
        }
    }

    [GlobalCleanup]
    public void Cleanup()
    {
        NativeMemory.AlignedFree((void*)_w);
        NativeMemory.AlignedFree((void*)_x);
        NativeMemory.AlignedFree((void*)_scalesW);
        NativeMemory.AlignedFree((void*)_scalesX);
    }

    [Benchmark(Baseline = true)]
    public float Full() => MatMul.VecDotQ8_0Avx2((byte*)_w, (byte*)_x, _blocks);

    [Benchmark]
    public float ConstScale()
    {
        byte* a = (byte*)_w, b = (byte*)_x;
        Vector256<float> acc = Vector256<float>.Zero;
        Vector256<short> ones = Vector256.Create((short)1);
        Vector256<float> scale = Vector256.Create(0.01f);   // hoisted, not per block
        for (int i = 0; i < _blocks; i++)
        {
            byte* ab = a + i * Q8_0BlockBytes;
            byte* bb = b + i * Q8_0BlockBytes;
            Vector256<sbyte> va = Unsafe.ReadUnaligned<Vector256<sbyte>>(ab + 2);
            Vector256<sbyte> vb = Unsafe.ReadUnaligned<Vector256<sbyte>>(bb + 2);
            Vector256<int> isum = MatMul.DotBytesToInt32(
                Avx2.Sign(va, va).AsByte(), Avx2.Sign(vb, va), ones);
            acc = Fma.MultiplyAdd(Avx.ConvertToVector256Single(isum), scale, acc);
        }
        return acc.GetElement(0);
    }

    [Benchmark]
    public int IntAccum()
    {
        byte* a = (byte*)_w, b = (byte*)_x;
        Vector256<int> acc = Vector256<int>.Zero;
        Vector256<short> ones = Vector256.Create((short)1);
        for (int i = 0; i < _blocks; i++)
        {
            byte* ab = a + i * Q8_0BlockBytes;
            byte* bb = b + i * Q8_0BlockBytes;
            Vector256<sbyte> va = Unsafe.ReadUnaligned<Vector256<sbyte>>(ab + 2);
            Vector256<sbyte> vb = Unsafe.ReadUnaligned<Vector256<sbyte>>(bb + 2);
            acc = Avx2.Add(acc, MatMul.DotBytesToInt32(
                Avx2.Sign(va, va).AsByte(), Avx2.Sign(vb, va), ones));
        }
        return acc.GetElement(0);
    }

    [Benchmark]
    public int DotOnly()
    {
        byte* a = (byte*)_w, b = (byte*)_x;
        Vector256<int> acc = Vector256<int>.Zero;
        Vector256<short> ones = Vector256.Create((short)1);
        for (int i = 0; i < _blocks; i++)
        {
            byte* ab = a + i * Q8_0BlockBytes;
            byte* bb = b + i * Q8_0BlockBytes;
            Vector256<byte> va = Unsafe.ReadUnaligned<Vector256<byte>>(ab + 2);
            Vector256<sbyte> vb = Unsafe.ReadUnaligned<Vector256<sbyte>>(bb + 2);
            acc = Avx2.Add(acc, MatMul.DotBytesToInt32(va, vb, ones));
        }
        return acc.GetElement(0);
    }

    /// <summary>
    /// Realistic target: block scales already converted to f32 and stored
    /// contiguously, so the per-block work is two float loads, a multiply and a
    /// broadcast — no fp16 conversion. This is what a precomputed-scale layout
    /// could actually achieve, as opposed to ConstScale's unreachable floor.
    /// </summary>
    [Benchmark]
    public float PrecomputedScales()
    {
        byte* a = (byte*)_w, b = (byte*)_x;
        float* sw = (float*)_scalesW, sx = (float*)_scalesX;
        Vector256<float> acc = Vector256<float>.Zero;
        Vector256<short> ones = Vector256.Create((short)1);
        for (int i = 0; i < _blocks; i++)
        {
            byte* ab = a + i * Q8_0BlockBytes;
            byte* bb = b + i * Q8_0BlockBytes;
            Vector256<sbyte> va = Unsafe.ReadUnaligned<Vector256<sbyte>>(ab + 2);
            Vector256<sbyte> vb = Unsafe.ReadUnaligned<Vector256<sbyte>>(bb + 2);
            Vector256<int> isum = MatMul.DotBytesToInt32(
                Avx2.Sign(va, va).AsByte(), Avx2.Sign(vb, va), ones);
            Vector256<float> scale = Vector256.Create(sw[i] * sx[i]);
            acc = Fma.MultiplyAdd(Avx.ConvertToVector256Single(isum), scale, acc);
        }
        return acc.GetElement(0);
    }

    /// <summary>
    /// Same, but with only the activation scales precomputed — the weight scale
    /// is still an fp16 read per block. This is the cheap half of the fix: the
    /// activation scales are shared by every row of a GEMV, so hoisting them
    /// costs no extra storage per weight.
    /// </summary>
    [Benchmark]
    public float PrecomputedActivationScaleOnly()
    {
        byte* a = (byte*)_w, b = (byte*)_x;
        float* sx = (float*)_scalesX;
        Vector256<float> acc = Vector256<float>.Zero;
        Vector256<short> ones = Vector256.Create((short)1);
        for (int i = 0; i < _blocks; i++)
        {
            byte* ab = a + i * Q8_0BlockBytes;
            byte* bb = b + i * Q8_0BlockBytes;
            float da = (float)Unsafe.ReadUnaligned<Half>(ab);
            Vector256<sbyte> va = Unsafe.ReadUnaligned<Vector256<sbyte>>(ab + 2);
            Vector256<sbyte> vb = Unsafe.ReadUnaligned<Vector256<sbyte>>(bb + 2);
            Vector256<int> isum = MatMul.DotBytesToInt32(
                Avx2.Sign(va, va).AsByte(), Avx2.Sign(vb, va), ones);
            Vector256<float> scale = Vector256.Create(da * sx[i]);
            acc = Fma.MultiplyAdd(Avx.ConvertToVector256Single(isum), scale, acc);
        }
        return acc.GetElement(0);
    }

    [Benchmark]
    public int LoadsOnly()
    {
        byte* a = (byte*)_w, b = (byte*)_x;
        Vector256<int> acc = Vector256<int>.Zero;
        for (int i = 0; i < _blocks; i++)
        {
            byte* ab = a + i * Q8_0BlockBytes;
            byte* bb = b + i * Q8_0BlockBytes;
            Vector256<int> va = Unsafe.ReadUnaligned<Vector256<int>>(ab + 2);
            Vector256<int> vb = Unsafe.ReadUnaligned<Vector256<int>>(bb + 2);
            acc = Avx2.Xor(acc, Avx2.Xor(va, vb));
        }
        return acc.GetElement(0);
    }
}
