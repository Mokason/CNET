using System;

namespace CNET.Cce;

/// <summary>
/// Managed checkpoint callback for <see cref="CceCallbacks.OnEpochEnd"/>.
/// Uses the native .cce model bundle through <see cref="CceCheckpoint.Save"/>.
/// </summary>
public sealed class CceCheckpointCallback
{
    private readonly CceModel _model;

    public CceCheckpointCallback(CceModel model, string directory, int everyNEpochs = 1, string prefix = "epoch")
    {
        ArgumentNullException.ThrowIfNull(model);
        if (string.IsNullOrWhiteSpace(directory)) throw new ArgumentException("Directory is required", nameof(directory));
        if (everyNEpochs <= 0) throw new ArgumentOutOfRangeException(nameof(everyNEpochs));
        if (string.IsNullOrWhiteSpace(prefix)) throw new ArgumentException("Prefix is required", nameof(prefix));

        _model = model;
        DirectoryPath = directory;
        EveryNEpochs = everyNEpochs;
        Prefix = prefix;
    }

    public string DirectoryPath { get; }
    public int EveryNEpochs { get; }
    public string Prefix { get; }
    public CceCheckpointInfo? LastCheckpoint { get; private set; }

    /// <summary>Saves a checkpoint when the one-based epoch number is divisible by <see cref="EveryNEpochs"/>.</summary>
    public bool OnEpochEnd(EpochInfo info)
    {
        int oneBasedEpoch = info.Epoch + 1;
        if (oneBasedEpoch % EveryNEpochs == 0)
        {
            LastCheckpoint = CceCheckpoint.Save(_model, DirectoryPath, $"{Prefix}-{oneBasedEpoch:D4}");
        }
        return true;
    }
}
