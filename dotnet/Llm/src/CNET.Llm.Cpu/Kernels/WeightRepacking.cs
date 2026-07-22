using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using CNET.Llm.Core.Configuration;

namespace CNET.Llm.Cpu.Kernels;

/// <summary>
/// Repacks quantized weight matrices from row-major to R4 interleaved layout.
/// Groups of 4 consecutive rows have their blocks interleaved column-by-column,
/// so that 4-row SIMD kernels read sequentially instead of striding across rows.
/// This improves cache utilization, TLB locality, and hardware prefetch effectiveness.
/// </summary>
public static unsafe class WeightRepacking
{
    /// <summary>Number of rows interleaved per group. Matches existing 4-row kernel batching.</summary>
    public const int InterleaveFactor = 4;

    /// <summary>
    /// Holds a repacked weight matrix with R4 interleaved layout.
    /// Full groups of 4 rows have their blocks interleaved. Tail rows (M % 4) are appended in original order.
    /// </summary>
    internal readonly struct RepackedWeight : IDisposable
    {
        /// <summary>64-byte aligned pointer to repacked data.</summary>
        public readonly nint Ptr;

        /// <summary>Number of complete 4-row groups (M / 4).</summary>
        public readonly int FullGroupCount;

        /// <summary>Number of leftover rows (M % 4).</summary>
        public readonly int TailRows;

        /// <summary>Number of quant blocks per row (K / groupSize).</summary>
        public readonly int BlocksPerRow;

        /// <summary>Byte size of one quant block.</summary>
        public readonly int BlockBytes;

        /// <summary>Total allocated bytes, including <see cref="ScalesPtr"/>.</summary>
        public readonly long AllocatedBytes;

        /// <summary>
        /// Block scales pre-converted from fp16 to f32, laid out to mirror the
        /// block ordering, or 0 when this quant type has no such plane.
        /// </summary>
        /// <remarks>
        /// Converting <c>Half</c> to <c>float</c> per block dominated the Q8_0
        /// dot product — roughly 72% of it, far outweighing the loads and the
        /// arithmetic. Hoisting the conversion to repack time costs 4 bytes per
        /// block (+11.8% for Q8_0) and is bit-identical, since converting a given
        /// <c>Half</c> once yields the same float as converting it every time.
        /// <para>
        /// Indexing mirrors the weight bytes exactly. Full groups:
        /// <c>(g * BlocksPerRow + b) * InterleaveFactor + r</c>. Tail rows start
        /// at <c>FullGroupCount * BlocksPerRow * InterleaveFactor</c> and are
        /// row-major: <c>+ r * BlocksPerRow + b</c>.
        /// </para>
        /// </remarks>
        public readonly nint ScalesPtr;

        public RepackedWeight(nint ptr, int fullGroupCount, int tailRows,
                              int blocksPerRow, int blockBytes, long allocatedBytes,
                              nint scalesPtr = 0)
        {
            Ptr = ptr;
            FullGroupCount = fullGroupCount;
            TailRows = tailRows;
            BlocksPerRow = blocksPerRow;
            BlockBytes = blockBytes;
            AllocatedBytes = allocatedBytes;
            ScalesPtr = scalesPtr;
        }

        /// <summary>Scales for the tail rows, which are stored row-major.</summary>
        public float* TailScales
        {
            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            get => (float*)ScalesPtr + (long)FullGroupCount * BlocksPerRow * InterleaveFactor;
        }

        /// <summary>Byte size of one original row: blocksPerRow * blockBytes.</summary>
        public int RowBytes
        {
            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            get => BlocksPerRow * BlockBytes;
        }

        /// <summary>Pointer to the start of tail rows (after all interleaved groups).</summary>
        public byte* TailPtr
        {
            [MethodImpl(MethodImplOptions.AggressiveInlining)]
            get => (byte*)Ptr + (long)FullGroupCount * InterleaveFactor * RowBytes;
        }

        public void Dispose()
        {
            if (Ptr != 0)
                NativeMemory.AlignedFree((void*)Ptr);
            if (ScalesPtr != 0)
                NativeMemory.AlignedFree((void*)ScalesPtr);
        }
    }

    /// <summary>
    /// Returns block byte size and group size for a given quantization type.
    /// Returns (0, 0) for unsupported types (F32, F16).
    /// </summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal static (int blockBytes, int groupSize) GetBlockInfo(QuantizationType qt) => qt switch
    {
        QuantizationType.Q8_0 => (34, 32),    // Q8_0BlockBytes, Q8_0GroupSize
        QuantizationType.Q5_0 => (22, 32),    // Q5_0BlockBytes, Q5_0GroupSize
        QuantizationType.Q4_K => (144, 256),  // Q4_K_BlockBytes, KQuantGroupSize
        QuantizationType.Q5_K => (176, 256),  // Q5_K_BlockBytes, KQuantGroupSize
        QuantizationType.Q6_K => (210, 256),  // Q6_K_BlockBytes, KQuantGroupSize
        _ => (0, 0),
    };

    /// <summary>
    /// Returns true if the given quant type supports R4 repacking.
    /// F32/F16 are not block-structured and are skipped.
    /// </summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static bool IsRepackable(QuantizationType qt) =>
        qt is QuantizationType.Q8_0 or QuantizationType.Q5_0
            or QuantizationType.Q4_K or QuantizationType.Q5_K or QuantizationType.Q6_K;

    /// <summary>
    /// Repacks a [M, K] quantized weight matrix from row-major to R4 interleaved layout.
    /// For each group of 4 rows, blocks are stored column-by-column:
    /// [row0_blk0][row1_blk0][row2_blk0][row3_blk0][row0_blk1][row1_blk1]...
    /// Tail rows (M % 4) are appended in original row-major order after the interleaved data.
    /// </summary>
    /// <param name="sourcePtr">Pointer to original row-major quantized weights.</param>
    /// <param name="buildScalePlane">
    /// Build the f32 Q8_0 scale plane. Defaults to <c>false</c>, and the default
    /// is deliberate — see the remarks.
    /// </param>
    /// <param name="qt">Quantization type of the weights.</param>
    /// <param name="m">Number of rows (output dimension).</param>
    /// <param name="k">Number of columns / elements per row (input dimension).</param>
    /// <returns>A <see cref="RepackedWeight"/> with the interleaved layout. Caller owns disposal.</returns>
    /// <remarks>
    /// <para><b>On <paramref name="buildScalePlane"/>.</b> Pre-converting the
    /// Q8_0 fp16 block scales to f32 makes the R4 decode kernel 1.5-1.7x faster
    /// in isolation — the conversion is roughly 72% of that kernel. It is also
    /// bit-identical, since converting a <c>Half</c> once yields the same float
    /// as converting it repeatedly.</para>
    /// <para>It is off by default anyway, because it makes real decode
    /// <em>slower</em>. The plane adds 11.8% to the bytes streamed per token, and
    /// decode is memory-bound: a 1.3 GB model at ~30 tok/s is already ~40 GB/s.
    /// Measured end to end on Llama-3.2-1B Q8_0, 5 runs each and no overlap
    /// between them: 29.08 tok/s with the plane against 30.72 without, a 5.3%
    /// regression. Microbenchmarks disagree because they hammer one tensor and
    /// get cache reuse that decode — which touches every weight byte exactly once
    /// per token — never sees.</para>
    /// <para>Turn it on only where the weights are genuinely cache-resident.</para>
    /// </remarks>
    internal static RepackedWeight RepackR4(nint sourcePtr, QuantizationType qt, int m, int k,
                                            bool buildScalePlane = false)
    {
        var (blockBytes, groupSize) = GetBlockInfo(qt);
        if (blockBytes == 0)
            throw new ArgumentException($"Quantization type {qt} does not support R4 repacking.", nameof(qt));

        if (k % groupSize != 0)
            throw new ArgumentException(
                $"k ({k}) must be a multiple of group size ({groupSize}) for {qt}.", nameof(k));

        int blocksPerRow = k / groupSize;
        int rowBytes = blocksPerRow * blockBytes;
        int fullGroups = m / InterleaveFactor;
        int tailRows = m % InterleaveFactor;

        if (m == 0)
            return default;

        // Total size = same as original (we're just rearranging blocks)
        long totalBytes = (long)m * rowBytes;
        nint destPtr = (nint)NativeMemory.AlignedAlloc((nuint)totalBytes, 64);

        byte* src = (byte*)sourcePtr;
        byte* dst = (byte*)destPtr;

        // Interleave full groups: for each group of 4 rows, write blocks column-by-column
        for (int g = 0; g < fullGroups; g++)
        {
            int baseRow = g * InterleaveFactor;
            byte* groupDst = dst + (long)g * InterleaveFactor * rowBytes;

            for (int b = 0; b < blocksPerRow; b++)
            {
                byte* colDst = groupDst + (long)b * InterleaveFactor * blockBytes;

                for (int r = 0; r < InterleaveFactor; r++)
                {
                    byte* srcBlock = src + (long)(baseRow + r) * rowBytes + (long)b * blockBytes;
                    byte* dstBlock = colDst + (long)r * blockBytes;
                    Buffer.MemoryCopy(srcBlock, dstBlock, blockBytes, blockBytes);
                }
            }
        }

        // Copy tail rows as-is (row-major)
        if (tailRows > 0)
        {
            int tailStartRow = fullGroups * InterleaveFactor;
            byte* tailSrc = src + (long)tailStartRow * rowBytes;
            byte* tailDst = dst + (long)fullGroups * InterleaveFactor * rowBytes;
            long tailBytes = (long)tailRows * rowBytes;
            Buffer.MemoryCopy(tailSrc, tailDst, tailBytes, tailBytes);
        }

        // Q8_0 carries exactly one fp16 scale at the head of each block, so its
        // scales can be pre-converted to f32 into a parallel plane. The k-quants
        // carry super-block scale structures instead and are left alone.
        nint scalesPtr = 0;
        long scaleBytes = 0;
        if (buildScalePlane && qt == QuantizationType.Q8_0)
        {
            scaleBytes = (long)m * blocksPerRow * sizeof(float);
            scalesPtr = (nint)NativeMemory.AlignedAlloc((nuint)scaleBytes, 64);
            float* scales = (float*)scalesPtr;

            for (int g = 0; g < fullGroups; g++)
            {
                byte* groupBase = dst + (long)g * InterleaveFactor * rowBytes;
                for (int b = 0; b < blocksPerRow; b++)
                {
                    byte* colBase = groupBase + (long)b * InterleaveFactor * blockBytes;
                    float* scaleBase = scales + ((long)g * blocksPerRow + b) * InterleaveFactor;
                    for (int r = 0; r < InterleaveFactor; r++)
                        scaleBase[r] = (float)Unsafe.ReadUnaligned<Half>(colBase + (long)r * blockBytes);
                }
            }

            float* tailScales = scales + (long)fullGroups * blocksPerRow * InterleaveFactor;
            byte* tailBase = dst + (long)fullGroups * InterleaveFactor * rowBytes;
            for (int r = 0; r < tailRows; r++)
                for (int b = 0; b < blocksPerRow; b++)
                {
                    byte* block = tailBase + (long)r * rowBytes + (long)b * blockBytes;
                    tailScales[(long)r * blocksPerRow + b] = (float)Unsafe.ReadUnaligned<Half>(block);
                }
        }

        return new RepackedWeight(destPtr, fullGroups, tailRows, blocksPerRow, blockBytes,
                                  totalBytes + scaleBytes, scalesPtr);
    }
}
