using System.Runtime.InteropServices;
using BenchmarkDotNet.Attributes;
using CNET.Llm.Core.Configuration;
using CNET.Llm.Cpu.Kernels;

namespace CNET.Llm.Benchmarks;

/// <summary>
/// Direct A/B of the R4 interleaved Q8_0 decode kernel with and without the
/// precomputed f32 weight-scale plane. Same weights, same activations, same
/// results — only whether the fp16 scale conversion happens per block or once at
/// repack time.
/// </summary>
[MemoryDiagnoser]
[SimpleJob(warmupCount: 3, iterationCount: 10)]
public unsafe class ScalePlaneBenchmarks
{
    private const int Q8_0BlockBytes = 34;
    private const int Q8_0GroupSize = 32;

    private WeightRepacking.RepackedWeight _packed;
    private nint _src, _xq, _result;
    private int _blocks;

    [Params(4096)]
    public int K { get; set; }

    // 256 rows keeps the weights cache-resident; 16384 pushes them past L3
    // (16384 x 4096 Q8_0 = 71 MB) so the run becomes memory-bound like real decode.
    [Params(256, 16384)]
    public int M { get; set; }

    [GlobalSetup]
    public void Setup()
    {
        var rng = new Random(42);
        _blocks = K / Q8_0GroupSize;
        int rowBytes = _blocks * Q8_0BlockBytes;
        _src = (nint)NativeMemory.AlignedAlloc((nuint)((long)M * rowBytes), 64);
        for (int row = 0; row < M; row++)
            for (int b = 0; b < _blocks; b++)
            {
                byte* block = (byte*)_src + (long)row * rowBytes + b * Q8_0BlockBytes;
                *(Half*)block = (Half)(rng.NextSingle() * 0.1f);
                for (int i = 0; i < Q8_0GroupSize; i++)
                    ((sbyte*)(block + 2))[i] = (sbyte)rng.Next(-127, 128);
            }

        _xq = (nint)NativeMemory.AlignedAlloc((nuint)rowBytes, 64);
        for (int b = 0; b < _blocks; b++)
        {
            byte* block = (byte*)_xq + b * Q8_0BlockBytes;
            *(Half*)block = (Half)(rng.NextSingle() * 0.2f);
            for (int i = 0; i < Q8_0GroupSize; i++)
                ((sbyte*)(block + 2))[i] = (sbyte)rng.Next(-127, 128);
        }

        _result = (nint)NativeMemory.AlignedAlloc((nuint)(M * sizeof(float)), 64);
        _packed = WeightRepacking.RepackR4(_src, QuantizationType.Q8_0, M, K, buildScalePlane: true);
    }

    [GlobalCleanup]
    public void Cleanup()
    {
        _packed.Dispose();
        NativeMemory.AlignedFree((void*)_src);
        NativeMemory.AlignedFree((void*)_xq);
        NativeMemory.AlignedFree((void*)_result);
    }

    [Benchmark(Baseline = true)]
    public void Fp16ScalesPerBlock() =>
        MatMul.ComputeRowsQ8_0Interleaved((byte*)_packed.Ptr, (byte*)_xq, (float*)_result,
            _packed.FullGroupCount, _packed.TailRows, _blocks, scales: null);

    [Benchmark]
    public void PrecomputedScalePlane() =>
        MatMul.ComputeRowsQ8_0Interleaved((byte*)_packed.Ptr, (byte*)_xq, (float*)_result,
            _packed.FullGroupCount, _packed.TailRows, _blocks, (float*)_packed.ScalesPtr);
}
