using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using CNET.Llm.Core.Configuration;
using CNET.Llm.Cpu.Kernels;
using CNET.Llm.Cpu.Threading;
using Xunit;

namespace CNET.Llm.Tests.Unit.Cpu.Kernels;

/// <summary>
/// Tests for the dequantize-to-f32 prefill path in Q8_0 GEMM.
///
/// Above <c>DequantF32TokenThreshold</c> tokens, GEMM expands each weight tile to
/// f32 once and runs the f32 GEMV, rather than quantizing activations and using
/// the Q8_0 kernel. That is faster, and it also changes results — the activation
/// round-trip through Q8_0 disappears. These tests pin the direction of that
/// change: the f32 path must be *closer* to exact arithmetic, never further.
/// </summary>
public sealed unsafe class MatMulDequantF32Tests
{
    private const int Q8_0BlockBytes = 34;
    private const int Q8_0GroupSize = 32;

    private static void FillRandomQ8_0Blocks(byte* ptr, int blockCount, Random rng)
    {
        for (int b = 0; b < blockCount; b++)
        {
            byte* block = ptr + b * Q8_0BlockBytes;
            *(Half*)block = (Half)(rng.NextSingle() * 0.1f);
            for (int i = 0; i < Q8_0GroupSize; i++)
                ((sbyte*)(block + 2))[i] = (sbyte)rng.Next(-127, 128);
        }
    }

    /// <summary>Exact <c>dequant(W) · x</c> in double precision — the value both paths approximate.</summary>
    private static double[] ExactReference(byte* weights, float[] input, int m, int k, int n)
    {
        int blockCount = k / Q8_0GroupSize;
        int rowBytes = blockCount * Q8_0BlockBytes;
        var result = new double[(long)n * m];

        for (int t = 0; t < n; t++)
            for (int row = 0; row < m; row++)
            {
                double sum = 0;
                for (int blk = 0; blk < blockCount; blk++)
                {
                    byte* block = weights + (long)row * rowBytes + blk * Q8_0BlockBytes;
                    double scale = (float)Unsafe.ReadUnaligned<Half>(block);
                    sbyte* q = (sbyte*)(block + 2);
                    for (int i = 0; i < Q8_0GroupSize; i++)
                        sum += scale * q[i] * input[(long)t * k + blk * Q8_0GroupSize + i];
                }
                result[(long)t * m + row] = sum;
            }
        return result;
    }

    private static double RelativeError(double[] exact, float[] actual)
    {
        double num = 0, den = 0;
        for (int i = 0; i < exact.Length; i++)
        {
            double d = actual[i] - exact[i];
            num += d * d;
            den += exact[i] * exact[i];
        }
        return Math.Sqrt(num / Math.Max(den, 1e-30));
    }

    /// <summary>
    /// The whole justification for the path switch: dropping the activation
    /// quantization round-trip must move results toward exact arithmetic.
    /// </summary>
    [Theory]
    [InlineData(16)]
    [InlineData(64)]
    public void DequantF32Path_IsCloserToExactThanQuantizedPath(int n)
    {
        var rng = new Random(31);
        const int m = 32, k = 512;
        int blockCount = k / Q8_0GroupSize;
        int rowBytes = blockCount * Q8_0BlockBytes;

        nint weightsPtr = (nint)NativeMemory.AlignedAlloc((nuint)((long)m * rowBytes), 64);
        try
        {
            for (int row = 0; row < m; row++)
                FillRandomQ8_0Blocks((byte*)weightsPtr + (long)row * rowBytes, blockCount, rng);

            float[] input = new float[(long)n * k];
            for (int i = 0; i < input.Length; i++) input[i] = rng.NextSingle() * 2f - 1f;

            double[] exact = ExactReference((byte*)weightsPtr, input, m, k, n);

            // n >= threshold: dequantize-to-f32 path.
            float[] f32Path = new float[(long)n * m];
            fixed (float* ip = input, op = f32Path)
                MatMul.GemmQ8_0((byte*)weightsPtr, ip, op, m, k, n);

            // Quantized path, forced by supplying pre-quantized activations.
            float[] quantPath = new float[(long)n * m];
            nint scratch = (nint)NativeMemory.AlignedAlloc((nuint)((long)n * rowBytes), 64);
            try
            {
                fixed (float* ip = input, op = quantPath)
                {
                    for (int t = 0; t < n; t++)
                        MatMul.QuantizeF32ToQ8_0(ip + (long)t * k, (byte*)scratch + (long)t * rowBytes, k);
                    MatMul.GemmQ8_0((byte*)weightsPtr, null, op, m, k, n, (byte*)scratch);
                }
            }
            finally
            {
                NativeMemory.AlignedFree((void*)scratch);
            }

            double f32Err = RelativeError(exact, f32Path);
            double quantErr = RelativeError(exact, quantPath);

            Assert.True(f32Err < quantErr,
                $"f32 path must be closer to exact: f32={f32Err:E3} quant={quantErr:E3}");
            // The f32 path carries only f32 rounding; the quantized path also
            // carries ~1/127 activation quantization error.
            Assert.True(f32Err < 1e-5, $"f32 path error unexpectedly large: {f32Err:E3}");
        }
        finally
        {
            NativeMemory.AlignedFree((void*)weightsPtr);
        }
    }

    /// <summary>Below the threshold the quantized kernel is kept, so results are unchanged.</summary>
    [Theory]
    [InlineData(1)]
    [InlineData(4)]
    [InlineData(15)]
    public void BelowThreshold_StillMatchesQuantizedGemv(int n)
    {
        var rng = new Random(37);
        const int m = 24, k = 256;
        int blockCount = k / Q8_0GroupSize;
        int rowBytes = blockCount * Q8_0BlockBytes;

        nint weightsPtr = (nint)NativeMemory.AlignedAlloc((nuint)((long)m * rowBytes), 64);
        try
        {
            for (int row = 0; row < m; row++)
                FillRandomQ8_0Blocks((byte*)weightsPtr + (long)row * rowBytes, blockCount, rng);

            float[] input = new float[(long)n * k];
            for (int i = 0; i < input.Length; i++) input[i] = rng.NextSingle() * 2f - 1f;

            float[] reference = new float[(long)n * m];
            float[] gemm = new float[(long)n * m];
            fixed (float* ip = input, sp = reference, gp = gemm)
            {
                for (int t = 0; t < n; t++)
                    MatMul.GemvQ8_0((byte*)weightsPtr, ip + (long)t * k, sp + (long)t * m, m, k);
                MatMul.GemmQ8_0((byte*)weightsPtr, ip, gp, m, k, n);
            }

            for (int i = 0; i < reference.Length; i++)
                Assert.Equal(reference[i], gemm[i], 1e-2f);
        }
        finally
        {
            NativeMemory.AlignedFree((void*)weightsPtr);
        }
    }

    /// <summary>Serial and pooled dequant paths must agree — they tile identically.</summary>
    [Theory]
    [InlineData(16)]
    [InlineData(33)]
    [InlineData(64)]
    public void ParallelDequantPath_MatchesSerial(int n)
    {
        var rng = new Random(41);
        const int m = 256, k = 512;
        int blockCount = k / Q8_0GroupSize;
        int rowBytes = blockCount * Q8_0BlockBytes;

        nint weightsPtr = (nint)NativeMemory.AlignedAlloc((nuint)((long)m * rowBytes), 64);
        try
        {
            for (int row = 0; row < m; row++)
                FillRandomQ8_0Blocks((byte*)weightsPtr + (long)row * rowBytes, blockCount, rng);

            float[] input = new float[(long)n * k];
            for (int i = 0; i < input.Length; i++) input[i] = rng.NextSingle() * 2f - 1f;

            float[] serial = new float[(long)n * m];
            float[] parallel = new float[(long)n * m];

            using var pool = new ComputeThreadPool(4, topology: null, new ThreadingConfig(4));
            fixed (float* ip = input, sp = serial, pp = parallel)
            {
                MatMul.GemmQ8_0((byte*)weightsPtr, ip, sp, m, k, n);
                MatMul.GemmQ8_0((byte*)weightsPtr, ip, pp, m, k, n, pool);
            }

            for (int i = 0; i < serial.Length; i++)
                Assert.Equal(serial[i], parallel[i], 1e-4f);
        }
        finally
        {
            NativeMemory.AlignedFree((void*)weightsPtr);
        }
    }

    /// <summary>Tile-boundary and remainder coverage for the dequantized path.</summary>
    [Theory]
    [InlineData(300, 512, 20)]
    [InlineData(17, 1024, 40)]
    [InlineData(64, 96, 17)]
    public void DequantPath_ShapesWithRemainders_MatchExact(int m, int k, int n)
    {
        var rng = new Random(43);
        int blockCount = k / Q8_0GroupSize;
        int rowBytes = blockCount * Q8_0BlockBytes;

        nint weightsPtr = (nint)NativeMemory.AlignedAlloc((nuint)((long)m * rowBytes), 64);
        try
        {
            for (int row = 0; row < m; row++)
                FillRandomQ8_0Blocks((byte*)weightsPtr + (long)row * rowBytes, blockCount, rng);

            float[] input = new float[(long)n * k];
            for (int i = 0; i < input.Length; i++) input[i] = rng.NextSingle() * 2f - 1f;

            double[] exact = ExactReference((byte*)weightsPtr, input, m, k, n);
            float[] gemm = new float[(long)n * m];
            fixed (float* ip = input, gp = gemm)
                MatMul.GemmQ8_0((byte*)weightsPtr, ip, gp, m, k, n);

            Assert.True(RelativeError(exact, gemm) < 1e-5);
        }
        finally
        {
            NativeMemory.AlignedFree((void*)weightsPtr);
        }
    }
}
