using System;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// Low-level handle for opening .cce archives and performing online adaptation
/// (the original cce_adapt / cce_infer path).
/// 
/// Use this when you want per-sample local learning with the full CCE forest/router machinery,
/// instead of the higher-level dataset training loop.
/// </summary>
public sealed class CceHandle : IDisposable
{
    private IntPtr _handle;
    private bool _disposed;
    private CceScheduler? _attachedScheduler;

    public static CceHandle Open(string archivePath)
    {
        if (string.IsNullOrWhiteSpace(archivePath))
            throw new ArgumentException("Archive path required", nameof(archivePath));

        var rc = CceNative.CceOpen(out var h, archivePath);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"Failed to open CCE archive '{archivePath}': {rc}");

        return new CceHandle { _handle = h };
    }

    public (int Label, float Confidence) Infer(ReadOnlySpan<float> input)
    {
        ThrowIfDisposed();
        float[] buf = input.ToArray();
        var rc = CceNative.CceInfer(_handle, buf, buf.Length, out int label, out float conf);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"Infer failed: {rc}");
        return (label, conf);
    }

    /// <summary>
    /// Performs one adaptation step (local learning or hybrid/exact depending on current mode).
    /// Respects any attached per-branch diff mode and scheduler.
    /// If a scheduler is attached, the passed lr is ignored and the scheduler's current LR is used instead.
    /// </summary>
    public void Adapt(ReadOnlySpan<float> input, int label, float lr = 0.01f)
    {
        ThrowIfDisposed();
        float effectiveLr = _attachedScheduler?.CurrentLr ?? lr;
        float[] buf = input.ToArray();
        var rc = CceNative.CceAdapt(_handle, buf, buf.Length, label, effectiveLr);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"Adapt failed: {rc}");
    }

    /// <summary>
    /// Adapt over a whole batch (convenience; calls Adapt per sample).
    /// Useful for streaming "DataLoader-style" online updates from a dataset.
    /// </summary>
    public void AdaptBatch(CceTrainingBatch batch, int[] labels)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(labels);
        if (labels.Length < batch.BatchSize) throw new ArgumentException("labels too small");

        for (int i = 0; i < batch.BatchSize; i++)
        {
            var sample = batch.Inputs.Slice(i * batch.InputDim, batch.InputDim);
            Adapt(sample, labels[i]);
        }
    }

    public void SetDiffMode(CceDiffMode mode)
    {
        ThrowIfDisposed();
        CceNative.CceSetDiffMode(_handle, (CceNative.CceDiffMode)mode);
    }

    public void SetScheduler(CceScheduler scheduler)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(scheduler);
        CceNative.CceSetScheduler(_handle, scheduler.DangerousHandle);
        _attachedScheduler = scheduler;
    }

    public void Dispose()
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            CceNative.CceClose(_handle);
            _handle = IntPtr.Zero;
        }
        _attachedScheduler = null;
        _disposed = true;
    }

    private void ThrowIfDisposed() => ObjectDisposedException.ThrowIf(_disposed || _handle == IntPtr.Zero, this);
}
