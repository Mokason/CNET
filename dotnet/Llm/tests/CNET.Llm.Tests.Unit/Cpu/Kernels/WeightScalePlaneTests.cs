using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using CNET.Llm.Core.Configuration;
using CNET.Llm.Cpu.Kernels;
using CNET.Llm.Cpu.Threading;
using Xunit;

namespace CNET.Llm.Tests.Unit.Cpu.Kernels;

/// <summary>
/// Tests the f32 weight-scale plane that repacking builds for Q8_0.
///
/// Converting a given <c>Half</c> once at repack time yields exactly the same
/// float as converting it on every use, so the scaled kernels must be
/// <em>bit</em>-identical to the fp16 ones — not merely close. These tests assert
/// exact equality, since anything looser would hide a real layout bug.
/// </summary>
public sealed unsafe class WeightScalePlaneTests
{
    private const int Q8_0BlockBytes = 34;
    private const int Q8_0GroupSize = 32;
    private const int InterleaveFactor = 4;

    private static nint AllocRandomQ8_0(int m, int k, int seed, out int rowBytes)
    {
        var rng = new Random(seed);
        int blocks = k / Q8_0GroupSize;
        rowBytes = blocks * Q8_0BlockBytes;
        nint p = (nint)NativeMemory.AlignedAlloc((nuint)((long)m * rowBytes), 64);
        for (int row = 0; row < m; row++)
            for (int b = 0; b < blocks; b++)
            {
                byte* block = (byte*)p + (long)row * rowBytes + b * Q8_0BlockBytes;
                *(Half*)block = (Half)(rng.NextSingle() * 0.1f);
                for (int i = 0; i < Q8_0GroupSize; i++)
                    ((sbyte*)(block + 2))[i] = (sbyte)rng.Next(-127, 128);
            }
        return p;
    }

    [Theory]
    [InlineData(8, 256)]     // exactly two groups, no tail
    [InlineData(9, 256)]     // one tail row
    [InlineData(11, 512)]    // three tail rows
    [InlineData(64, 1024)]
    public void ScalePlane_MatchesTheFp16ScalesItMirrors(int m, int k)
    {
        nint src = AllocRandomQ8_0(m, k, seed: 5, out int rowBytes);
        try
        {
            using var packed = WeightRepacking.RepackR4(src, QuantizationType.Q8_0, m, k, buildScalePlane: true);
            Assert.NotEqual(0, packed.ScalesPtr);

            int blocks = k / Q8_0GroupSize;
            float* scales = (float*)packed.ScalesPtr;
            byte* dst = (byte*)packed.Ptr;

            // Full groups: scale index mirrors the interleaved byte index.
            for (int g = 0; g < packed.FullGroupCount; g++)
                for (int b = 0; b < blocks; b++)
                    for (int r = 0; r < InterleaveFactor; r++)
                    {
                        byte* block = dst + (long)g * InterleaveFactor * rowBytes
                                    + (long)b * InterleaveFactor * Q8_0BlockBytes
                                    + (long)r * Q8_0BlockBytes;
                        float expected = (float)Unsafe.ReadUnaligned<Half>(block);
                        float actual = scales[((long)g * blocks + b) * InterleaveFactor + r];
                        Assert.Equal(expected, actual);   // exact
                    }

            // Tail rows are row-major.
            byte* tailBase = dst + (long)packed.FullGroupCount * InterleaveFactor * rowBytes;
            float* tailScales = packed.TailScales;
            for (int r = 0; r < packed.TailRows; r++)
                for (int b = 0; b < blocks; b++)
                {
                    float expected = (float)Unsafe.ReadUnaligned<Half>(
                        tailBase + (long)r * rowBytes + (long)b * Q8_0BlockBytes);
                    Assert.Equal(expected, tailScales[(long)r * blocks + b]);
                }
        }
        finally
        {
            NativeMemory.AlignedFree((void*)src);
        }
    }

    [Theory]
    [InlineData(8, 256)]
    [InlineData(9, 256)]
    [InlineData(37, 512)]
    [InlineData(256, 1024)]
    public void ScaledKernel_IsBitIdenticalToFp16Kernel(int m, int k)
    {
        nint src = AllocRandomQ8_0(m, k, seed: 11, out _);
        int blocks = k / Q8_0GroupSize;
        nint xq = (nint)NativeMemory.AlignedAlloc((nuint)(blocks * Q8_0BlockBytes), 64);
        try
        {
            var rng = new Random(13);
            for (int b = 0; b < blocks; b++)
            {
                byte* block = (byte*)xq + b * Q8_0BlockBytes;
                *(Half*)block = (Half)(rng.NextSingle() * 0.2f);
                for (int i = 0; i < Q8_0GroupSize; i++)
                    ((sbyte*)(block + 2))[i] = (sbyte)rng.Next(-127, 128);
            }

            using var packed = WeightRepacking.RepackR4(src, QuantizationType.Q8_0, m, k, buildScalePlane: true);

            var withScales = new float[m];
            var withoutScales = new float[m];
            fixed (float* a = withScales, b = withoutScales)
            {
                MatMul.ComputeRowsQ8_0Interleaved((byte*)packed.Ptr, (byte*)xq, a,
                    packed.FullGroupCount, packed.TailRows, blocks, (float*)packed.ScalesPtr);
                MatMul.ComputeRowsQ8_0Interleaved((byte*)packed.Ptr, (byte*)xq, b,
                    packed.FullGroupCount, packed.TailRows, blocks, scales: null);
            }

            for (int i = 0; i < m; i++)
                Assert.Equal(withoutScales[i], withScales[i]);   // exact, not approximate
        }
        finally
        {
            NativeMemory.AlignedFree((void*)src);
            NativeMemory.AlignedFree((void*)xq);
        }
    }

    [Theory]
    [InlineData(256, 512)]
    [InlineData(300, 1024)]
    public void ParallelScaledPath_IsBitIdenticalToSerial(int m, int k)
    {
        nint src = AllocRandomQ8_0(m, k, seed: 17, out _);
        int blocks = k / Q8_0GroupSize;
        nint xq = (nint)NativeMemory.AlignedAlloc((nuint)(blocks * Q8_0BlockBytes), 64);
        try
        {
            var rng = new Random(19);
            for (int b = 0; b < blocks; b++)
            {
                byte* block = (byte*)xq + b * Q8_0BlockBytes;
                *(Half*)block = (Half)(rng.NextSingle() * 0.2f);
                for (int i = 0; i < Q8_0GroupSize; i++)
                    ((sbyte*)(block + 2))[i] = (sbyte)rng.Next(-127, 128);
            }

            using var packed = WeightRepacking.RepackR4(src, QuantizationType.Q8_0, m, k, buildScalePlane: true);
            using var pool = new ComputeThreadPool(4, topology: null, new ThreadingConfig(4));

            var serial = new float[m];
            var parallel = new float[m];
            fixed (float* a = serial, b = parallel)
            {
                MatMul.ComputeRowsQ8_0Interleaved((byte*)packed.Ptr, (byte*)xq, a,
                    packed.FullGroupCount, packed.TailRows, blocks, (float*)packed.ScalesPtr);
                MatMul.ComputeRowsQ8_0Interleaved((byte*)packed.Ptr, (byte*)xq, b,
                    packed.FullGroupCount, packed.TailRows, blocks, pool, (float*)packed.ScalesPtr);
            }

            for (int i = 0; i < m; i++)
                Assert.Equal(serial[i], parallel[i]);
        }
        finally
        {
            NativeMemory.AlignedFree((void*)src);
            NativeMemory.AlignedFree((void*)xq);
        }
    }

    /// <summary>The plane is Q8_0-only; k-quants carry super-block scales instead.</summary>
    [Theory]
    [InlineData(QuantizationType.Q4_K, 256)]
    [InlineData(QuantizationType.Q5_0, 32)]
    public void NonQ8_0_HasNoScalePlane(QuantizationType qt, int groupSize)
    {
        var (blockBytes, _) = (qt == QuantizationType.Q4_K) ? (144, 256) : (22, 32);
        const int m = 8;
        int k = groupSize * 4;
        int blocks = k / groupSize;
        nint src = (nint)NativeMemory.AlignedAlloc((nuint)((long)m * blocks * blockBytes), 64);
        try
        {
            new Span<byte>((void*)src, m * blocks * blockBytes).Clear();
            using var packed = WeightRepacking.RepackR4(src, qt, m, k, buildScalePlane: true);
            Assert.Equal(0, packed.ScalesPtr);
        }
        finally
        {
            NativeMemory.AlignedFree((void*)src);
        }
    }

    /// <summary>The memory cost is the tradeoff this change accepted; pin it.</summary>
    [Fact]
    public void ScalePlane_CostsElevenPointEightPercent()
    {
        const int m = 64, k = 4096;
        nint src = AllocRandomQ8_0(m, k, seed: 23, out _);
        try
        {
            using var packed = WeightRepacking.RepackR4(src, QuantizationType.Q8_0, m, k, buildScalePlane: true);
            long weightBytes = (long)m * (k / Q8_0GroupSize) * Q8_0BlockBytes;
            long scaleBytes = packed.AllocatedBytes - weightBytes;

            Assert.Equal((long)m * (k / Q8_0GroupSize) * sizeof(float), scaleBytes);
            double overhead = (double)scaleBytes / weightBytes;
            Assert.InRange(overhead, 0.117, 0.119);   // 4/34
        }
        finally
        {
            NativeMemory.AlignedFree((void*)src);
        }
    }
}
