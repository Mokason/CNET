namespace CNET.Cce;

/// <summary>Per-epoch information passed to training callbacks.</summary>
public readonly record struct EpochInfo(int Epoch, double TrainLoss, float? ValAccuracy, float CurrentLr, double? ValLoss = null);

/// <summary>Per-batch information passed to training callbacks.</summary>
public readonly record struct BatchInfo(int Epoch, int Batch, int BatchSize, double TrainLoss, float CurrentLr);

/// <summary>Managed training trace captured from <see cref="CceModel.FitHistory"/>.</summary>
public sealed record CceTrainingHistory(
    System.Collections.Generic.IReadOnlyList<EpochInfo> Epochs,
    System.Collections.Generic.IReadOnlyList<BatchInfo> Batches,
    double FinalLoss)
{
    public int EpochCount => Epochs.Count;
    public int BatchCount => Batches.Count;
    public EpochInfo? LastEpoch => Epochs.Count > 0 ? Epochs[^1] : null;
    public BatchInfo? LastBatch => Batches.Count > 0 ? Batches[^1] : null;
}

/// <summary>
/// Training callbacks for the managed <see cref="CceModel.Fit"/> loop.
/// <see cref="OnEpochEnd"/> returns false to request early stop (true/null continues).
/// <see cref="OnBatchEnd"/> returns false to request early stop after the current batch.
/// </summary>
public sealed record CceCallbacks
{
    public System.Func<BatchInfo, bool>? OnBatchEnd { get; init; }
    public System.Func<EpochInfo, bool>? OnEpochEnd { get; init; }

    /// <summary>
    /// Combines multiple callback sets. All handlers run; the combined handler returns false
    /// if any handler requests stop.
    /// </summary>
    public static CceCallbacks Combine(params CceCallbacks?[] callbacks) =>
        new()
        {
            OnBatchEnd = info =>
            {
                bool keepGoing = true;
                foreach (var callback in callbacks ?? System.Array.Empty<CceCallbacks?>())
                {
                    if (callback?.OnBatchEnd is { } handler)
                    {
                        if (!handler(info))
                            keepGoing = false;
                    }
                }
                return keepGoing;
            },
            OnEpochEnd = info =>
            {
                bool keepGoing = true;
                foreach (var callback in callbacks ?? System.Array.Empty<CceCallbacks?>())
                {
                    if (callback?.OnEpochEnd is { } handler)
                    {
                        if (!handler(info))
                            keepGoing = false;
                    }
                }
                return keepGoing;
            }
        };
}
