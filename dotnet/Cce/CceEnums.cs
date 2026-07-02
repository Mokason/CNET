namespace CNET.Cce;

/// <summary>
/// Differentiation mode used by the CCE engine.
/// </summary>
public enum CceDiffMode
{
    /// <summary>
    /// Default local learning (NoProp / DFA / Free Energy style). Preserves compositionality.
    /// </summary>
    Local = 0,

    /// <summary>
    /// Exact backprop on the last N layers (configurable tail), local credit assignment before.
    /// </summary>
    Hybrid = 1,

    /// <summary>
    /// Full backpropagation through the cascade using the optional autograd tape (only for small exact tails/heads).
    /// Local learning remains the default for the vast majority of specialists.
    /// </summary>
    Exact = 2,
}

/// <summary>
/// Learning rate scheduler types supported by the native engine.
/// </summary>
public enum CceSchedulerType
{
    Cosine = 0,
    Warmup = 1,
    Plateau = 2,
    Step = 3,
}

/// <summary>Output loss used during training (host-side for metrics + config; core dispatch uses classify flag).</summary>
public enum CceLossType
{
    MeanSquaredError = 0,
    CrossEntropy = 1,
    /// <summary>Binary cross-entropy (expects targets in [0,1] or one-hot style for dim=1/2 cases).</summary>
    BinaryCrossEntropy = 2,
    /// <summary>Huber-like (smooth L1) for robust regression.</summary>
    Huber = 3,
}

/// <summary>
/// Configuration for a training run.
/// </summary>
public sealed record CceTrainingConfig
{
    public int MaxEpochs { get; init; } = 100;
    public float TargetLoss { get; init; } = 0.01f;
    public bool ShuffleEachEpoch { get; init; } = true;
    public CceDiffMode DiffMode { get; init; } = CceDiffMode.Local;
    public CceSchedulerConfig? Scheduler { get; init; }
    public int? VerboseEveryNEpochs { get; init; } = null;
    public CceLossType Loss { get; init; } = CceLossType.MeanSquaredError;
    /// <summary>Learner knobs. Null leaves the engine default.</summary>
    public float? GoodnessThreshold { get; init; }
    public float? DfaStrength { get; init; }
    public float? GradClip { get; init; }

    /// <summary>
    /// When DiffMode == Exact, use the small reverse-mode autograd tape for the tail instead of DFA.
    /// Default false keeps the previous exact path behavior.
    /// </summary>
    public bool UseAutogradExactTail { get; init; } = false;
}

/// <summary>
/// Scheduler configuration passed to the native side.
/// </summary>
public sealed record CceSchedulerConfig(
    CceSchedulerType Type = CceSchedulerType.Cosine,
    float InitialLr = 0.01f,
    int WarmupEpochs = 5,
    float DecayFactor = 0.5f,
    int StepSize = 20,
    float PlateauFactor = 0.5f,
    int PlateauPatience = 8);

/// <summary>Typed composition relation between two branches (matches C conn_types).</summary>
public enum CceConnectionType
{
    Sub = 0,
    Refines = 1,
    Composes = 2,
    Specializes = 3,
}
