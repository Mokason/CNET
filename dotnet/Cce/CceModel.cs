using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// High-level .NET 10 wrapper over a native CCE model.
/// Supports adding forests (by native handle for advanced scenarios),
/// attaching schedulers and diff modes, and full training + inference using
/// the compositional CCE engine (local learning + optional hybrid/exact tails).
/// </summary>
public sealed class CceModel : IDisposable
{
    private IntPtr _handle;
    private bool _disposed;
    private CceScheduler? _attachedScheduler;
    private bool _ownsAttachedScheduler;

    // Keeps managed CceForest wrappers added via Add() reachable for the model's
    // lifetime. The native model stores their raw pointers long-term, so without
    // this a forest could be finalized (its SafeHandle releasing the native forest)
    // while the model still references it -> use-after-free.
    private readonly List<CceForest> _addedForests = new();

    public string Name { get; }

    /// <summary>
    /// Creates a new empty CCE model.
    /// </summary>
    public CceModel(string name)
    {
        if (string.IsNullOrWhiteSpace(name))
            throw new ArgumentException("Model name is required", nameof(name));

        var result = CceNative.CceModelCreate(out _handle, name);
        if (result != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"Failed to create CceModel: {result}");

        Name = name;
    }

    /// <summary>
    /// Adds a forest as a named module of the model. Preferred over the raw-pointer overload.
    /// The model keeps the <see cref="CceForest"/> alive for its own lifetime; do NOT
    /// dispose the forest while this model is still in use (the native model holds its pointer).
    /// </summary>
    public void Add(CceForest forest, string name)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(forest);
        if (forest.IsInvalid) throw new ArgumentException("Forest handle is invalid", nameof(forest));
        var rc = CceNative.CceModelAddForest(_handle, forest.DangerousHandle, name ?? "unnamed");
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"Add failed: {rc}");
        _addedForests.Add(forest);   // keep reachable so it isn't finalized out from under the native model
    }

    /// <summary>
    /// Adds a native forest (cce_forest*) to the model.
    /// Most users will load .cce archives on the C side or use pre-built perceptual forests.
    /// The forest pointer must remain valid for the lifetime of the model (or until removed).
    /// </summary>
    [Obsolete("Use Add(CceForest, name). Raw IntPtr is for advanced/interop scenarios only.")]
    public void AddForest(IntPtr nativeForestHandle, string forestName)
    {
        ThrowIfDisposed();
        if (nativeForestHandle == IntPtr.Zero)
            throw new ArgumentNullException(nameof(nativeForestHandle));

        var rc = CceNative.CceModelAddForest(_handle, nativeForestHandle, forestName ?? "unnamed");
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"AddForest failed: {rc}");
    }

    /// <summary>
    /// Sets the global differentiation mode (can be overridden per-branch inside forests).
    /// </summary>
    public void SetDiffMode(CceDiffMode mode)
    {
        ThrowIfDisposed();
        CceNative.CceModelSetDiffMode(_handle, (CceNative.CceDiffMode)mode);
    }

    /// <summary>
    /// Attaches a scheduler. The model keeps the scheduler reachable, but callers
    /// must not dispose it while the model is still using it.
    /// </summary>
    public void SetScheduler(CceScheduler scheduler)
    {
        AttachScheduler(scheduler, ownsScheduler: false);
    }

    private void AttachScheduler(CceScheduler scheduler, bool ownsScheduler)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(scheduler);

        var rc = CceNative.CceModelSetScheduler(_handle, scheduler.DangerousHandle);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"SetScheduler failed: {rc}");

        if (_ownsAttachedScheduler)
            _attachedScheduler?.Dispose();
        _attachedScheduler = scheduler;
        _ownsAttachedScheduler = ownsScheduler;
    }

    /// <summary>
    /// High-level training loop. Dispatches batches to the correct forests using names/router.
    /// Respects per-branch diff modes and the attached scheduler.
    /// </summary>
    public double Train(CceDataset dataset, CceTrainingConfig? config = null)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(dataset);

        config ??= new CceTrainingConfig();

        ApplyTrainingConfig(config);

        if (config.Scheduler is not null)
        {
            var sched = new CceScheduler(config.Scheduler);
            AttachScheduler(sched, ownsScheduler: true);
        }

        if (config.ShuffleEachEpoch)
            dataset.Shuffle();

        double finalLoss = CceNative.CceModelTrain(
            _handle,
            dataset.NativeHandle,
            (nuint)config.MaxEpochs,
            config.TargetLoss);

        return finalLoss;
    }

    private void ApplyTrainingConfig(CceTrainingConfig config)
    {
        CceNative.CceModelSetLoss(_handle, (int)config.Loss);
        if (config.GoodnessThreshold is not null || config.DfaStrength is not null || config.GradClip is not null)
        {
            CceNative.CceModelSetLearnParams(
                _handle,
                config.GoodnessThreshold ?? 0f,
                config.DfaStrength ?? 0f,
                config.GradClip ?? 0f);
        }
        SetDiffMode(config.DiffMode);
        CceNative.CceModelSetUseAutograd(_handle, config.UseAutogradExactTail ? 1 : 0);
    }

    /// <summary>
    /// Managed training loop with per-epoch callbacks and optional validation accuracy.
    /// Uses the per-batch path so callbacks can log / early-stop / checkpoint. The C
    /// <see cref="Train"/> remains the no-callback fast path.
    /// </summary>
    public double Fit(CceDataset train, CceTrainingConfig? config = null,
                      CceCallbacks? callbacks = null, CceDataset? validation = null)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(train);
        config ??= new CceTrainingConfig();

        ApplyTrainingConfig(config);

        if (config.Scheduler is not null)
        {
            var sched = new CceScheduler(config.Scheduler);
            AttachScheduler(sched, ownsScheduler: true);
        }

        double lastLoss = double.PositiveInfinity;
        bool stopRequested = false;
        for (int epoch = 0; epoch < config.MaxEpochs; epoch++)
        {
            if (config.ShuffleEachEpoch) train.Shuffle();
            train.Reset();

            double sum = 0; int nb = 0;
            while (train.NextBatch(out var batch))
            {
                double batchLoss = TrainBatch(batch);
                sum += batchLoss;
                float batchLr = _attachedScheduler?.CurrentLr ?? 0f;
                var batchInfo = new BatchInfo(epoch, nb, batch.BatchSize, batchLoss, batchLr);
                nb++;
                if (!(callbacks?.OnBatchEnd?.Invoke(batchInfo) ?? true))
                {
                    stopRequested = true;
                    break;
                }
            }
            lastLoss = nb > 0 ? sum / nb : lastLoss;

            double? valLoss = validation is null ? null : CceMetrics.Loss(this, validation, config.Loss);
            float? valAcc = validation is null ? null : CceMetrics.Accuracy(this, validation);
            float lr = _attachedScheduler?.CurrentLr ?? 0f;
            var info = new EpochInfo(epoch, lastLoss, valAcc, lr, valLoss);

            bool keepGoing = callbacks?.OnEpochEnd?.Invoke(info) ?? true;
            if (!keepGoing) break;
            if (stopRequested) break;
            if (lastLoss >= 0 && lastLoss < config.TargetLoss) break;
        }
        return lastLoss;
    }

    /// <summary>
    /// Runs the managed training loop and returns per-epoch history for plotting, logging, or diagnostics.
    /// This is a host-side convenience over <see cref="Fit"/>; the C core remains focused on cascade learning.
    /// </summary>
    public CceTrainingHistory FitHistory(CceDataset train, CceTrainingConfig? config = null,
                                         CceCallbacks? callbacks = null, CceDataset? validation = null)
    {
        var epochs = new List<EpochInfo>();
        var batches = new List<BatchInfo>();
        var chainedCallbacks = new CceCallbacks
        {
            OnBatchEnd = info =>
            {
                batches.Add(info);
                return callbacks?.OnBatchEnd?.Invoke(info) ?? true;
            },
            OnEpochEnd = info =>
            {
                epochs.Add(info);
                return callbacks?.OnEpochEnd?.Invoke(info) ?? true;
            }
        };

        double finalLoss = Fit(train, config, chainedCallbacks, validation);
        return new CceTrainingHistory(epochs.ToArray(), batches.ToArray(), finalLoss);
    }

    /// <summary>
    /// Train a single batch (advanced / custom loops). Returns batch loss.
    /// </summary>
    public double TrainBatch(CceTrainingBatch batch)
    {
        ThrowIfDisposed();

        // Convert managed batch view to native pointer struct (only valid during the call).
        unsafe
        {
            fixed (float* inPtr = batch.Inputs)
            fixed (float* tgtPtr = batch.Targets)
            {
                var nativeBatch = new CceNative.CceBatchNative
                {
                    Inputs = (IntPtr)inPtr,
                    Targets = (IntPtr)tgtPtr,
                    BatchSize = (nuint)batch.BatchSize,
                    InDim = (nuint)batch.InputDim,
                    OutDim = (nuint)batch.OutputDim,
                    Index = (nuint)batch.Index
                };

                return CceNative.CceModelTrainBatch(_handle, ref nativeBatch);
            }
        }
    }

    /// <summary>
    /// Batch inference. Labels and confidences are written to the provided arrays.
    /// </summary>
    public void InferBatch(CceTrainingBatch batch, int[] labels, float[] confidences)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(labels);
        ArgumentNullException.ThrowIfNull(confidences);

        unsafe
        {
            fixed (float* inPtr = batch.Inputs)
            {
                var nativeBatch = new CceNative.CceBatchNative
                {
                    Inputs = (IntPtr)inPtr,
                    Targets = IntPtr.Zero,
                    BatchSize = (nuint)batch.BatchSize,
                    InDim = (nuint)batch.InputDim,
                    OutDim = (nuint)batch.OutputDim,
                    Index = (nuint)batch.Index
                };

                var rc = CceNative.CceModelInferBatch(_handle, ref nativeBatch, labels, confidences);
                if (rc != CceNative.CceResult.Ok)
                    throw new InvalidOperationException($"InferBatch failed: {rc}");
            }
        }
    }

    /// <summary>
    /// Raw forward through the routed specialist. Returns the cascade output vector
    /// before argmax/snap-style inference reduces it to a label.
    /// </summary>
    public float[] Forward(ReadOnlySpan<float> input, int outputDim)
    {
        ThrowIfDisposed();
        if (input.IsEmpty) throw new ArgumentException("Input cannot be empty", nameof(input));
        if (outputDim <= 0) throw new ArgumentOutOfRangeException(nameof(outputDim));

        float[] inBuf = input.ToArray();
        float[] outBuf = new float[outputDim];
        var rc = CceNative.CceModelForward(_handle, inBuf, inBuf.Length, outBuf, outputDim, out int actualDim);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"Forward failed: {rc}");

        if (actualDim == outputDim) return outBuf;
        Array.Resize(ref outBuf, actualDim);
        return outBuf;
    }

    /// <summary>
    /// Raw batched forward. The returned array is row-major with <paramref name="outputDim"/>
    /// values per sample.
    /// </summary>
    public float[] ForwardBatch(CceTrainingBatch batch, int outputDim, out int actualOutputDim)
    {
        ThrowIfDisposed();
        if (outputDim <= 0) throw new ArgumentOutOfRangeException(nameof(outputDim));

        float[] outputs = new float[batch.BatchSize * outputDim];
        unsafe
        {
            fixed (float* inPtr = batch.Inputs)
            {
                var nativeBatch = new CceNative.CceBatchNative
                {
                    Inputs = (IntPtr)inPtr,
                    Targets = IntPtr.Zero,
                    BatchSize = (nuint)batch.BatchSize,
                    InDim = (nuint)batch.InputDim,
                    OutDim = (nuint)outputDim,
                    Index = (nuint)batch.Index
                };

                var rc = CceNative.CceModelForwardBatch(_handle, ref nativeBatch, outputs, outputDim, out actualOutputDim);
                if (rc != CceNative.CceResult.Ok)
                    throw new InvalidOperationException($"ForwardBatch failed: {rc}");
            }
        }

        if (actualOutputDim <= 0)
            throw new InvalidOperationException($"ForwardBatch returned invalid output dim {actualOutputDim}");
        if (actualOutputDim > outputDim)
            throw new InvalidOperationException($"ForwardBatch returned output dim {actualOutputDim} larger than capacity {outputDim}");
        if (actualOutputDim < outputDim)
        {
            float[] compact = new float[batch.BatchSize * actualOutputDim];
            for (int s = 0; s < batch.BatchSize; s++)
            {
                Array.Copy(
                    outputs,
                    s * outputDim,
                    compact,
                    s * actualOutputDim,
                    actualOutputDim);
            }
            return compact;
        }
        return outputs;
    }

    public float[] ForwardBatch(CceTrainingBatch batch, int outputDim) =>
        ForwardBatch(batch, outputDim, out _);

    /// <summary>
    /// Predicts a class label by taking argmax over raw routed cascade outputs.
    /// This keeps classification ergonomics in .NET while preserving the CCE core's raw specialist output.
    /// For lowest latency with many specialists, call SealForInference() on the model (or Seal() on forests) first.
    /// </summary>
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)]
    public int Predict(ReadOnlySpan<float> input, int outputDim)
    {
        float[] logits = Forward(input, outputDim);
        return ArgMax(logits);
    }

    /// <summary>
    /// Zero-allocation-ish fast path for the common case when you have pre-allocated buffers.
    /// Still routes through the model. Best used after sealing forests.
    /// </summary>
    public void PredictFast(ReadOnlySpan<float> input, int outputDim, Span<int> outLabel, Span<float> outConf)
    {
        ThrowIfDisposed();
        if (outLabel.IsEmpty || outConf.IsEmpty) throw new ArgumentException("Output spans required");
        // Use the existing batch/single but for 1 element we reuse Infer for simplicity.
        var (lbl, conf) = Infer(input);
        outLabel[0] = lbl;
        outConf[0] = conf;
    }

    /// <summary>
    /// Returns normalized probabilities by applying a numerically stable softmax to raw routed outputs.
    /// </summary>
    public float[] PredictProbabilities(ReadOnlySpan<float> input, int outputDim)
    {
        float[] logits = Forward(input, outputDim);
        SoftmaxInPlace(logits);
        return logits;
    }

    /// <summary>
    /// Predicts one class label per sample by taking argmax over each row of batched raw outputs.
    /// </summary>
    public int[] PredictBatch(CceTrainingBatch batch, int outputDim)
    {
        ThrowIfDisposed();
        if (outputDim <= 0) throw new ArgumentOutOfRangeException(nameof(outputDim));

        float[] logits = ForwardBatch(batch, outputDim, out int actualOutputDim);
        if (actualOutputDim <= 0)
            throw new InvalidOperationException($"ForwardBatch returned invalid output dim {actualOutputDim}");

        int[] labels = new int[batch.BatchSize];
        for (int s = 0; s < batch.BatchSize; s++)
        {
            labels[s] = ArgMax(logits.AsSpan(s * actualOutputDim, actualOutputDim));
        }
        return labels;
    }

    /// <summary>
    /// Returns row-major softmax probabilities for each sample in a batch.
    /// </summary>
    public float[] PredictProbabilitiesBatch(CceTrainingBatch batch, int outputDim, out int actualOutputDim)
    {
        ThrowIfDisposed();
        if (outputDim <= 0) throw new ArgumentOutOfRangeException(nameof(outputDim));

        float[] logits = ForwardBatch(batch, outputDim, out actualOutputDim);
        if (actualOutputDim <= 0)
            throw new InvalidOperationException($"ForwardBatch returned invalid output dim {actualOutputDim}");

        for (int s = 0; s < batch.BatchSize; s++)
        {
            SoftmaxInPlace(logits.AsSpan(s * actualOutputDim, actualOutputDim));
        }
        return logits;
    }

    public float[] PredictProbabilitiesBatch(CceTrainingBatch batch, int outputDim) =>
        PredictProbabilitiesBatch(batch, outputDim, out _);

    /// <summary>
    /// Evaluates classification quality over a dataset using raw routed output argmax.
    /// This is a convenience wrapper over <see cref="CceMetrics.ClassificationReport"/>.
    /// </summary>
    public CceClassificationReport Evaluate(CceDataset dataset, int topK = 1) =>
        CceMetrics.ClassificationReport(this, dataset, topK);

    /// <summary>
    /// Evaluates regression quality over raw routed outputs.
    /// This is a convenience wrapper over <see cref="CceMetrics.RegressionReport"/>.
    /// </summary>
    public CceRegressionReport EvaluateRegression(CceDataset dataset) =>
        CceMetrics.RegressionReport(this, dataset);

    /// <summary>
    /// Computes mean dataset loss from raw routed outputs.
    /// This is a convenience wrapper over <see cref="CceMetrics.Loss"/>.
    /// </summary>
    public double EvaluateLoss(CceDataset dataset, CceLossType lossType = CceLossType.MeanSquaredError) =>
        CceMetrics.Loss(this, dataset, lossType);

    /// <summary>
    /// Attempts to enable GPU acceleration for this model's training loops.
    /// 
    /// Requires the native cce.dll to have been built with CUDA support (CCE_USE_CUDA=1 + CUDA toolkit).
    /// Falls back silently to CPU if CUDA is unavailable. This is the primary "PyTorch-like" 
    /// acceleration hook for small-to-medium specialist cascades.
    /// 
    /// Call this before Train/Fit. For advanced control use the lower-level GPU context APIs.
    /// </summary>
    public bool TryUseGpu()
    {
        ThrowIfDisposed();
        try
        {
            // Try explicit CUDA first (preferred for speed on training)
            var rc = CceNative.CceGpuInitCuda(out IntPtr ctx);
            if (rc == CceNative.CceResult.Ok && ctx != IntPtr.Zero)
            {
                var setRc = CceNative.CceModelSetGpuOwned(_handle, ctx);
                return setRc == CceNative.CceResult.Ok;
            }

            // cce_gpu_init is a CPU context, not an accelerator. Do not attach
            // it and report a false-positive GPU enablement.
            return false;
        }
        catch (EntryPointNotFoundException)
        {
            // The current cce.dll was not built with GPU symbols exported (common).
            // Users who want GPU must rebuild with the proper CUDA flags.
            // The OpenCL model-kernel backend is cce_clgemm.c, a separate path.
            return false;
        }
        catch (DllNotFoundException)
        {
            return false;
        }
    }

    /// <summary>
    /// Sets the desired device. CUDA/Auto attempt CUDA and otherwise remain on
    /// CPU. OpenCL uses the separate cce_clgemm tensor API and is not available
    /// through this high-level model method.
    /// </summary>
    public void SetDevice(CceDevice device)
    {
        ThrowIfDisposed();
        if (device == CceDevice.OpenCl)
            throw new NotSupportedException("CceDevice.OpenCl requires the separate native cce_clgemm API.");
        if (device == CceDevice.Cuda || device == CceDevice.Auto)
            TryUseGpu();
    }

    /// <summary>
    /// Seals all attached forests for optimal zero-copy inference performance.
    /// Call this after adding all specialists and before high-volume inference.
    /// </summary>
    public void SealForInference()
    {
        ThrowIfDisposed();
        // Note: we don't have direct access to internal forests list, but user can seal forests before Add,
        // or we can expose a way. For now, recommend sealing individual forests.
        // To make useful, we can track added forests.
        foreach (var f in _addedForests)
        {
            if (!f.IsInvalid)
            {
                try { f.Seal(); } catch { /* already sealed or error ok */ }
            }
        }
    }

    /// <summary>
    /// Single vector inference (convenience).
    /// </summary>
    public (int Label, float Confidence) Infer(ReadOnlySpan<float> input)
    {
        ThrowIfDisposed();
        if (input.IsEmpty)
            throw new ArgumentException("Input cannot be empty");

        // We have to copy to a float[] because the P/Invoke signature currently uses float[].
        // In a future version this can be improved with ReadOnlySpan + custom marshalling.
        float[] buf = input.ToArray();
        var rc = CceNative.CceModelInfer(_handle, buf, buf.Length, out int label, out float conf);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"Infer failed: {rc}");

        return (label, conf);
    }

    /// <summary>Saves the entire model (forests + scheduler + config) to one self-contained .cce bundle.</summary>
    public void Save(string path)
    {
        ThrowIfDisposed();
        if (string.IsNullOrWhiteSpace(path)) throw new ArgumentException("Path required", nameof(path));
        var rc = CceNative.CceModelSave(_handle, path);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"Save failed: {rc}");
    }

    /// <summary>Loads a model previously written by <see cref="Save"/>. Returns a fully RAM-resident model that owns its forests.</summary>
    public static CceModel Load(string path)
    {
        if (string.IsNullOrWhiteSpace(path)) throw new ArgumentException("Path required", nameof(path));
        var model = new CceModel("loaded");
        var rc = CceNative.CceModelLoad(model._handle, path);
        if (rc != CceNative.CceResult.Ok)
        {
            model.Dispose();
            throw new InvalidOperationException($"Load failed: {rc}");
        }
        return model;
    }

    public void Dispose()
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            CceNative.CceModelDestroy(_handle);
            _handle = IntPtr.Zero;
        }
        if (_ownsAttachedScheduler)
            _attachedScheduler?.Dispose();
        _attachedScheduler = null;
        _ownsAttachedScheduler = false;
        _disposed = true;
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed || _handle == IntPtr.Zero, this);
    }

    private static int ArgMax(ReadOnlySpan<float> values)
    {
        if (values.IsEmpty) throw new ArgumentException("Values cannot be empty", nameof(values));

        int best = 0;
        float bestValue = values[0];
        for (int i = 1; i < values.Length; i++)
        {
            if (values[i] > bestValue)
            {
                best = i;
                bestValue = values[i];
            }
        }
        return best;
    }

    private static void SoftmaxInPlace(Span<float> values)
    {
        if (values.IsEmpty) throw new ArgumentException("Values cannot be empty", nameof(values));

        float max = values[0];
        for (int i = 1; i < values.Length; i++)
            if (values[i] > max) max = values[i];

        double sum = 0.0;
        for (int i = 0; i < values.Length; i++)
        {
            double e = Math.Exp(values[i] - max);
            values[i] = (float)e;
            sum += e;
        }

        if (sum <= 0.0 || double.IsNaN(sum) || double.IsInfinity(sum))
            throw new InvalidOperationException("Softmax normalization failed");

        for (int i = 0; i < values.Length; i++)
            values[i] = (float)(values[i] / sum);
    }
}
