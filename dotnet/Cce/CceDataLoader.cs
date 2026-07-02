using System;
using System.Collections;
using System.Collections.Generic;

namespace CNET.Cce;

/// <summary>
/// A safe, materialized (owned copy) batch.
/// Unlike <see cref="CceTrainingBatch"/> (a ref struct view valid only for the current iterator position),
/// a materialized batch can be stored, queued, or reused across iterator advances.
/// </summary>
public readonly struct CceMaterializedBatch
{
    public float[] Inputs { get; }
    public float[] Targets { get; }
    public int BatchSize { get; }
    public int InputDim { get; }
    public int OutputDim { get; }
    public int Index { get; }

    internal CceMaterializedBatch(float[] inputs, float[] targets, int batchSize, int inDim, int outDim, int index)
    {
        Inputs = inputs;
        Targets = targets;
        BatchSize = batchSize;
        InputDim = inDim;
        OutputDim = outDim;
        Index = index;
    }

    /// <summary>Returns a lightweight view wrapper over this materialized data (for APIs that accept CceTrainingBatch).</summary>
    public CceTrainingBatch AsView() => new CceTrainingBatch(
        Inputs.AsSpan(0, BatchSize * InputDim),
        Targets.AsSpan(0, BatchSize * OutputDim),
        BatchSize, InputDim, OutputDim, Index);

    public override string ToString() =>
        $"CceMaterializedBatch[BatchSize={BatchSize}, InDim={InputDim}, OutDim={OutputDim}, Index={Index}]";
}

/// <summary>
/// Lightweight DataLoader-style iterator over a <see cref="CceDataset"/>.
/// 
/// Two usage modes:
/// - Default: yields safe <see cref="CceMaterializedBatch"/> copies (can be stored/queued).
/// - Hot path: use <see cref="StreamRefBatches"/> (or the dataset directly) for zero-copy ref-struct views.
/// 
/// This gives a familiar PyTorch DataLoader experience while respecting CCE's zero-alloc design where possible.
/// The loader is disposable only to mirror dataset lifetime expectations; it does not own the underlying dataset.
/// </summary>
public sealed class CceDataLoader : IEnumerable<CceMaterializedBatch>, IEnumerable, IDisposable
{
    private readonly CceDataset _dataset;
    private readonly bool _materialize;
    private readonly bool _autoShuffleOnReset;
    private bool _disposed;

    /// <param name="dataset">The source dataset. The loader does not take ownership.</param>
    /// <param name="materialize">When true (default), yields owned copies safe to retain. When false, falls back to ref views (use StreamRefBatches for clarity).</param>
    /// <param name="autoShuffleOnReset">If true, shuffles the dataset each time the iterator restarts.</param>
    public CceDataLoader(CceDataset dataset, bool materialize = true, bool autoShuffleOnReset = false)
    {
        _dataset = dataset ?? throw new ArgumentNullException(nameof(dataset));
        _materialize = materialize;
        _autoShuffleOnReset = autoShuffleOnReset;
    }

    /// <summary>
    /// Returns an enumerator that yields safe materialized batches.
    /// Each batch is an independent copy and remains valid after the dataset iterator moves.
    /// </summary>
    public IEnumerator<CceMaterializedBatch> GetEnumerator()
    {
        ThrowIfDisposed();
        if (_autoShuffleOnReset)
            _dataset.Shuffle();
        _dataset.Reset();

        while (_dataset.NextBatch(out var view))
        {
            if (_materialize)
            {
                yield return Materialize(view);
            }
            else
            {
                // Non-materialized path returns a copy of the view data anyway for collection safety.
                // For true ref semantics use StreamRefBatches().
                yield return Materialize(view);
            }
        }
    }

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    /// <summary>
    /// Zero-copy streaming using a callback (the only safe way to deliver ref structs from an iterator-like API).
    /// The view is only valid for the duration of the callback.
    /// </summary>
    public void StreamRefBatches(Action<CceTrainingBatch> onBatch, bool shuffleFirst = false)
    {
        ThrowIfDisposed();
        if (shuffleFirst) _dataset.Shuffle();
        _dataset.Reset();

        while (_dataset.NextBatch(out var batch))
        {
            onBatch(batch);
        }
    }

    /// <summary>Convenience: run an action over every batch (materialized by default).</summary>
    public void ForEach(Action<CceMaterializedBatch> action)
    {
        foreach (var b in this) action(b);
    }

    /// <summary>Convenience: run an action using raw ref batches (zero-copy where possible).</summary>
    public void ForEachRef(Action<CceTrainingBatch> action, bool shuffleFirst = false)
    {
        StreamRefBatches(action, shuffleFirst);
    }

    private static CceMaterializedBatch Materialize(CceTrainingBatch view)
    {
        int inBytes = view.BatchSize * view.InputDim;
        int outBytes = view.BatchSize * view.OutputDim;

        float[] ins = new float[inBytes];
        float[] tgts = new float[outBytes];

        view.Inputs.CopyTo(ins);
        view.Targets.CopyTo(tgts);

        return new CceMaterializedBatch(ins, tgts, view.BatchSize, view.InputDim, view.OutputDim, view.Index);
    }

    public void Dispose()
    {
        _disposed = true;
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }
}

public static class CceDatasetExtensions
{
    /// <summary>
    /// Returns a DataLoader over this dataset (materialized batches by default for safety).
    /// </summary>
    public static CceDataLoader AsDataLoader(this CceDataset dataset, bool materialize = true, bool autoShuffleOnReset = false)
        => new CceDataLoader(dataset, materialize, autoShuffleOnReset);

    /// <summary>
    /// Convenience for a one-shot pass that yields materialized batches.
    /// </summary>
    public static IEnumerable<CceMaterializedBatch> ToMaterializedBatches(this CceDataset dataset, bool shuffle = false)
    {
        using var loader = new CceDataLoader(dataset, materialize: true, autoShuffleOnReset: shuffle);
        foreach (var b in loader) yield return b;
    }
}
