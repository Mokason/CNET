using System;
using System.Runtime.InteropServices;

namespace CNET.Cce;

/// <summary>
/// A zero-copy (or pinned) view of a training/inference batch.
/// Valid only while the source <see cref="CceDataset"/> is alive and the
/// current iterator position has not advanced.
/// </summary>
public readonly ref struct CceTrainingBatch
{
    public readonly ReadOnlySpan<float> Inputs;
    public readonly ReadOnlySpan<float> Targets;

    public readonly int BatchSize;
    public readonly int InputDim;
    public readonly int OutputDim;
    public readonly int Index;           // global starting index in the dataset

    internal CceTrainingBatch(
        ReadOnlySpan<float> inputs,
        ReadOnlySpan<float> targets,
        int batchSize,
        int inputDim,
        int outputDim,
        int index)
    {
        Inputs = inputs;
        Targets = targets;
        BatchSize = batchSize;
        InputDim = inputDim;
        OutputDim = outputDim;
        Index = index;
    }

    public override string ToString() =>
        $"CceTrainingBatch[BatchSize={BatchSize}, InDim={InputDim}, OutDim={OutputDim}, Index={Index}]";
}
