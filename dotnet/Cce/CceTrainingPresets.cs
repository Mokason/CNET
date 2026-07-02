namespace CNET.Cce;

/// <summary>
/// Managed training presets for common CCE workflows. These are convenience recipes over
/// <see cref="CceTrainingConfig"/>, not hidden native policy.
/// </summary>
public static class CceTrainingPresets
{
    /// <summary>Fast compositional local-learning baseline.</summary>
    public static CceTrainingConfig LocalFast(
        int maxEpochs = 25,
        float targetLoss = 0.01f,
        float initialLr = 0.01f) =>
        new()
        {
            MaxEpochs = maxEpochs,
            TargetLoss = targetLoss,
            DiffMode = CceDiffMode.Local,
            Loss = CceLossType.MeanSquaredError,
            Scheduler = new CceSchedulerConfig(CceSchedulerType.Cosine, initialLr),
            ShuffleEachEpoch = true,
        };

    /// <summary>Hybrid mode for classifier-quality training while preserving local learning before the tail.</summary>
    public static CceTrainingConfig HybridQuality(
        int maxEpochs = 60,
        float targetLoss = 0.005f,
        float initialLr = 0.015f) =>
        new()
        {
            MaxEpochs = maxEpochs,
            TargetLoss = targetLoss,
            DiffMode = CceDiffMode.Hybrid,
            Loss = CceLossType.CrossEntropy,
            Scheduler = new CceSchedulerConfig(CceSchedulerType.Cosine, initialLr),
            ShuffleEachEpoch = true,
            GradClip = 1.0f,
        };

    /// <summary>Short exact-tail/full-exact fine-tuning pass for small specialists or final heads.</summary>
    public static CceTrainingConfig ExactFineTune(
        int maxEpochs = 10,
        float targetLoss = 0.002f,
        float initialLr = 0.005f) =>
        new()
        {
            MaxEpochs = maxEpochs,
            TargetLoss = targetLoss,
            DiffMode = CceDiffMode.Exact,
            Loss = CceLossType.CrossEntropy,
            Scheduler = new CceSchedulerConfig(CceSchedulerType.Warmup, initialLr, WarmupEpochs: 2),
            ShuffleEachEpoch = true,
            GradClip = 1.0f,
        };
}
