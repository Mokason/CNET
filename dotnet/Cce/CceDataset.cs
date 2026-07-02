using System;
using System.Runtime.InteropServices;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>Owns train and validation datasets created from one split operation.</summary>
public sealed class CceDatasetSplit : IDisposable
{
    public CceDatasetSplit(CceDataset train, CceDataset validation)
    {
        Train = train ?? throw new ArgumentNullException(nameof(train));
        Validation = validation ?? throw new ArgumentNullException(nameof(validation));
    }

    public CceDataset Train { get; }
    public CceDataset Validation { get; }

    public void Dispose()
    {
        Train.Dispose();
        Validation.Dispose();
    }
}

/// <summary>
/// .NET 10 friendly wrapper for CCE training data.
/// Supports both copied ownership (FromArrays) and zero-copy wrapping (WrapArrays / WrapMemory).
/// 
/// The dataset yields <see cref="CceTrainingBatch"/> ref structs containing Spans into the
/// underlying memory (native or pinned managed).
/// 
/// This is the primary way to feed perceptual (7-seg, glyph, grid) and symbolic (tile memory)
/// data into a <see cref="CceModel"/>.
/// </summary>
public sealed class CceDataset : IDisposable
{
    private IntPtr _handle;
    private GCHandle? _pinnedInputs;
    private GCHandle? _pinnedTargets;
    private bool _disposed;

    /// <summary>
    /// Total number of samples.
    /// </summary>
    public int SampleCount { get; private set; }

    public int InputDim { get; private set; }
    public int OutputDim { get; private set; }
    public int BatchSize { get; private set; }

    internal IntPtr NativeHandle => _handle;

    private CceDataset() { }

    /// <summary>
    /// Creates a dataset by copying the data into native memory (recommended for most cases).
    /// The managed arrays can be discarded after this call.
    /// </summary>
    public static CceDataset FromArrays(
        ReadOnlySpan<float> inputs,
        ReadOnlySpan<float> targets,
        int nSamples,
        int inDim,
        int outDim,
        int batchSize)
    {
        Validate(inDim, outDim, nSamples, batchSize);
        CceTransforms.ValidateMatrix(inputs.Length, nSamples, inDim);
        CceTransforms.ValidateMatrix(targets.Length, nSamples, outDim);

        // Copy into fresh managed arrays so we can safely pass to native which will copy again
        float[] inputCopy = inputs.ToArray();
        float[] targetCopy = targets.ToArray();

        var ds = new CceDataset
        {
            SampleCount = nSamples,
            InputDim = inDim,
            OutputDim = outDim,
            BatchSize = batchSize
        };

        var rc = CceNative.CceDatasetFromArrays(
            out ds._handle,
            inputCopy,
            targetCopy,
            (nuint)nSamples,
            (nuint)inDim,
            (nuint)outDim,
            (nuint)batchSize);

        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"FromArrays failed: {rc}");

        return ds;
    }

    /// <summary>
    /// Creates a dataset from flat inputs and integer class labels. Labels are one-hot encoded
    /// and inputs can be row-normalized before they are copied into the native C dataset.
    /// </summary>
    public static CceDataset FromLabels(
        ReadOnlySpan<float> inputs,
        ReadOnlySpan<int> labels,
        int nSamples,
        int inDim,
        int classCount,
        int batchSize,
        bool normalizeRowsL2 = false,
        float labelSmoothing = 0f)
    {
        Validate(inDim, classCount, nSamples, batchSize);
        if (labels.Length != nSamples)
            throw new ArgumentException("Label count must equal nSamples.", nameof(labels));
        CceTransforms.ValidateMatrix(inputs.Length, nSamples, inDim);

        float[] preparedInputs = normalizeRowsL2
            ? CceTransforms.NormalizeRowsL2(inputs, nSamples, inDim)
            : inputs.ToArray();
        float[] targets = CceTransforms.OneHot(labels, classCount, labelSmoothing);

        return FromArrays(preparedInputs, targets, nSamples, inDim, classCount, batchSize);
    }

    /// <summary>
    /// Splits flat input/target rows into copied train and validation datasets.
    /// Use <paramref name="shuffle"/> with <paramref name="seed"/> for deterministic validation splits.
    /// </summary>
    public static CceDatasetSplit SplitFromArrays(
        ReadOnlySpan<float> inputs,
        ReadOnlySpan<float> targets,
        int nSamples,
        int inDim,
        int outDim,
        int batchSize,
        float validationFraction = 0.2f,
        bool shuffle = true,
        int seed = 42)
    {
        Validate(inDim, outDim, nSamples, batchSize);
        if (nSamples < 2)
            throw new ArgumentOutOfRangeException(nameof(nSamples), "A train/validation split requires at least two samples.");
        CceTransforms.ValidateMatrix(inputs.Length, nSamples, inDim);
        CceTransforms.ValidateMatrix(targets.Length, nSamples, outDim);
        if (validationFraction <= 0f || validationFraction >= 1f)
            throw new ArgumentOutOfRangeException(nameof(validationFraction), "Validation fraction must be between 0 and 1.");

        int validationCount = Math.Clamp((int)Math.Round(nSamples * validationFraction), 1, nSamples - 1);
        int trainCount = nSamples - validationCount;

        int[] order = new int[nSamples];
        for (int i = 0; i < order.Length; i++) order[i] = i;
        if (shuffle) ShuffleOrder(order, seed);

        float[] trainInputs = new float[trainCount * inDim];
        float[] trainTargets = new float[trainCount * outDim];
        float[] validationInputs = new float[validationCount * inDim];
        float[] validationTargets = new float[validationCount * outDim];

        for (int i = 0; i < trainCount; i++)
        {
            CopyRow(inputs, order[i], inDim, trainInputs, i);
            CopyRow(targets, order[i], outDim, trainTargets, i);
        }
        for (int i = 0; i < validationCount; i++)
        {
            int source = order[trainCount + i];
            CopyRow(inputs, source, inDim, validationInputs, i);
            CopyRow(targets, source, outDim, validationTargets, i);
        }

        return new CceDatasetSplit(
            FromArrays(trainInputs, trainTargets, trainCount, inDim, outDim, batchSize),
            FromArrays(validationInputs, validationTargets, validationCount, inDim, outDim, batchSize));
    }

    /// <summary>
    /// Splits flat inputs and integer labels into copied train and validation datasets.
    /// Labels are one-hot encoded after splitting.
    /// </summary>
    public static CceDatasetSplit SplitFromLabels(
        ReadOnlySpan<float> inputs,
        ReadOnlySpan<int> labels,
        int nSamples,
        int inDim,
        int classCount,
        int batchSize,
        float validationFraction = 0.2f,
        bool normalizeRowsL2 = false,
        bool shuffle = true,
        int seed = 42,
        float labelSmoothing = 0f)
    {
        Validate(inDim, classCount, nSamples, batchSize);
        CceTransforms.ValidateMatrix(inputs.Length, nSamples, inDim);
        if (labels.Length != nSamples)
            throw new ArgumentException("Label count must equal nSamples.", nameof(labels));

        float[] preparedInputs = normalizeRowsL2
            ? CceTransforms.NormalizeRowsL2(inputs, nSamples, inDim)
            : inputs.ToArray();
        float[] targets = CceTransforms.OneHot(labels, classCount, labelSmoothing);

        return SplitFromArrays(
            preparedInputs,
            targets,
            nSamples,
            inDim,
            classCount,
            batchSize,
            validationFraction,
            shuffle,
            seed);
    }

    /// <summary>
    /// Wraps existing managed arrays without copying.
    /// Caller MUST keep the original arrays alive for the entire lifetime of the dataset
    /// and any CceTrainingBatch views obtained from it.
    /// </summary>
    public static CceDataset WrapArrays(
        float[] inputs,
        float[] targets,
        int nSamples,
        int inDim,
        int outDim,
        int batchSize)
    {
        Validate(inDim, outDim, nSamples, batchSize);
        ArgumentNullException.ThrowIfNull(inputs);
        ArgumentNullException.ThrowIfNull(targets);
        CceTransforms.ValidateMatrix(inputs.Length, nSamples, inDim);
        CceTransforms.ValidateMatrix(targets.Length, nSamples, outDim);

        var ds = new CceDataset
        {
            SampleCount = nSamples,
            InputDim = inDim,
            OutputDim = outDim,
            BatchSize = batchSize
        };

        // Pin so native pointers stay valid
        ds._pinnedInputs = GCHandle.Alloc(inputs, GCHandleType.Pinned);
        ds._pinnedTargets = GCHandle.Alloc(targets, GCHandleType.Pinned);

        var rc = CceNative.CceDatasetWrapArrays(
            out ds._handle,
            inputs,
            targets,
            (nuint)nSamples,
            (nuint)inDim,
            (nuint)outDim,
            (nuint)batchSize);

        if (rc != CceNative.CceResult.Ok)
        {
            ds.DisposePins();
            throw new InvalidOperationException($"WrapArrays failed: {rc}");
        }

        return ds;
    }

    /// <summary>
    /// Wrap using ReadOnlyMemory (more modern). Pins the underlying memory.
    /// </summary>
    public static CceDataset WrapMemory(
        ReadOnlyMemory<float> inputs,
        ReadOnlyMemory<float> targets,
        int nSamples,
        int inDim,
        int outDim,
        int batchSize)
    {
        // We need the actual arrays for the current P/Invoke surface.
        // For production this can be upgraded to custom marshalling or unsafe pointer overloads.
        return WrapArrays(inputs.ToArray(), targets.ToArray(), nSamples, inDim, outDim, batchSize);
    }

    private static void Validate(int inDim, int outDim, int nSamples, int batchSize)
    {
        if (inDim <= 0 || outDim <= 0 || nSamples <= 0 || batchSize <= 0)
            throw new ArgumentOutOfRangeException("Dimensions and counts must be positive");
        if ((long)nSamples * inDim > int.MaxValue || (long)nSamples * outDim > int.MaxValue)
            throw new ArgumentException("Dataset too large for current marshalling");
    }

    private static void CopyRow(ReadOnlySpan<float> source, int sourceRow, int width, Span<float> destination, int destinationRow)
    {
        source.Slice(sourceRow * width, width).CopyTo(destination.Slice(destinationRow * width, width));
    }

    private static void ShuffleOrder(Span<int> order, int seed)
    {
        var random = new Random(seed);
        for (int i = order.Length - 1; i > 0; i--)
        {
            int j = random.Next(i + 1);
            (order[i], order[j]) = (order[j], order[i]);
        }
    }

    /// <summary>
    /// Advances the iterator and returns the next batch as a ref struct view.
    /// The returned batch is only valid until the next call to NextBatch or Reset.
    /// </summary>
    public bool NextBatch(out CceTrainingBatch batch)
    {
        ThrowIfDisposed();

        var native = new CceNative.CceBatchNative();
        var rc = CceNative.CceDatasetNextBatch(_handle, ref native);

        if (rc == CceNative.CceResult.ErrNotFound)
        {
            batch = default;
            return false;
        }

        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"NextBatch failed: {rc}");

        unsafe
        {
            var inputSpan = new ReadOnlySpan<float>((float*)native.Inputs, (int)(native.BatchSize * native.InDim));
            var targetSpan = new ReadOnlySpan<float>((float*)native.Targets, (int)(native.BatchSize * native.OutDim));

            batch = new CceTrainingBatch(
                inputSpan,
                targetSpan,
                (int)native.BatchSize,
                (int)native.InDim,
                (int)native.OutDim,
                (int)native.Index);
        }

        return true;
    }

    public void Reset()
    {
        ThrowIfDisposed();
        CceNative.CceDatasetReset(_handle);
    }

    public void Shuffle()
    {
        ThrowIfDisposed();
        CceNative.CceDatasetShuffle(_handle);
    }

    public void Dispose()
    {
        if (!_disposed)
        {
            if (_handle != IntPtr.Zero)
            {
                CceNative.CceDatasetDestroy(_handle);
                _handle = IntPtr.Zero;
            }
            DisposePins();
            _disposed = true;
        }
    }

    private void DisposePins()
    {
        if (_pinnedInputs.HasValue)
        {
            _pinnedInputs.Value.Free();
            _pinnedInputs = null;
        }
        if (_pinnedTargets.HasValue)
        {
            _pinnedTargets.Value.Free();
            _pinnedTargets = null;
        }
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    // ============================================================
    // Synthetic dataset generators (for quick benchmarks, examples,
    // and "DataLoader-like" usage without external data files).
    // ============================================================

    /// <summary>
    /// Creates a simple multi-class gaussian blobs classification dataset.
    /// Useful for quick smoke tests and micro-benchmarks that exercise routing + local learning.
    /// </summary>
    public static CceDataset FromGaussianBlobs(
        int nSamples,
        int inDim,
        int classCount,
        int batchSize,
        double clusterStd = 1.0,
        int? seed = 42)
    {
        if (classCount < 2) throw new ArgumentOutOfRangeException(nameof(classCount));
        var rng = seed.HasValue ? new Random(seed.Value) : new Random();

        float[] inputs = new float[nSamples * inDim];
        int[] labels = new int[nSamples];

        // Place class centers somewhat spread in the hypercube
        float[,] centers = new float[classCount, inDim];
        for (int c = 0; c < classCount; c++)
            for (int d = 0; d < inDim; d++)
                centers[c, d] = (float)((rng.NextDouble() - 0.5) * 4.0);

        for (int i = 0; i < nSamples; i++)
        {
            int cls = i % classCount;
            labels[i] = cls;
            for (int d = 0; d < inDim; d++)
            {
                double noise = (rng.NextDouble() - 0.5) * 2.0 * clusterStd;
                inputs[i * inDim + d] = centers[cls, d] + (float)noise;
            }
        }

        return FromLabels(inputs, labels, nSamples, inDim, classCount, batchSize, normalizeRowsL2: true);
    }

    /// <summary>
    /// Simple XOR-like or parity synthetic (low dim, non-linear boundary) for testing capacity.
    /// inDim should be small (e.g. 2-8); targets are 2-class.
    /// </summary>
    public static CceDataset FromXorParity(int nSamples, int inDim, int batchSize, int? seed = 123)
    {
        var rng = seed.HasValue ? new Random(seed.Value) : new Random();
        float[] inputs = new float[nSamples * inDim];
        int[] labels = new int[nSamples];

        for (int i = 0; i < nSamples; i++)
        {
            int parity = 0;
            for (int d = 0; d < inDim; d++)
            {
                float v = (float)(rng.NextDouble() * 2 - 1);
                inputs[i * inDim + d] = v;
                if (v > 0) parity ^= 1;
            }
            labels[i] = parity;
        }
        return FromLabels(inputs, labels, nSamples, inDim, classCount: 2, batchSize: batchSize, normalizeRowsL2: false);
    }

    /// <summary>
    /// Simple linear regression target: y = Wx + b + noise (single output by default).
    /// </summary>
    public static CceDataset ForLinearRegression(int nSamples, int inDim, int outDim, int batchSize, double noiseStd = 0.1, int? seed = 7)
    {
        var rng = seed.HasValue ? new Random(seed.Value) : new Random();
        float[] inputs = new float[nSamples * inDim];
        float[] targets = new float[nSamples * outDim];

        // Random weights
        float[,] W = new float[outDim, inDim];
        float[] b = new float[outDim];
        for (int o = 0; o < outDim; o++)
        {
            b[o] = (float)(rng.NextDouble() - 0.5) * 0.5f;
            for (int d = 0; d < inDim; d++)
                W[o, d] = (float)(rng.NextDouble() - 0.5);
        }

        for (int i = 0; i < nSamples; i++)
        {
            for (int d = 0; d < inDim; d++)
                inputs[i * inDim + d] = (float)(rng.NextDouble() * 2 - 1);

            for (int o = 0; o < outDim; o++)
            {
                double y = b[o];
                for (int d = 0; d < inDim; d++)
                    y += W[o, d] * inputs[i * inDim + d];
                y += (rng.NextDouble() - 0.5) * 2 * noiseStd;
                targets[i * outDim + o] = (float)y;
            }
        }

        return FromArrays(inputs, targets, nSamples, inDim, outDim, batchSize);
    }
}
