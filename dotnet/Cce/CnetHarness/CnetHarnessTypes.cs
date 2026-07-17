// Public option/result types for the CNET .NET inference harness.
//
// These types are the managed projection of the versioned C ABI declared in
// include/cnet_harness.h. They are UTF-8-safe and expose adapter, route
// uncertainty, effective sampling profile, token counts, and timings.

using System;

namespace CNET.Cce.CnetHarness;

/// <summary>Result codes surfaced from the plugin's C ABI.</summary>
public enum CnetHarnessStatus
{
    Ok = 0,
    InvalidArgument = -1,
    ModelLoadFailed = -2,
    BackendFailure = -3,
    InvalidState = -4,
    Internal = -5,
}

/// <summary>Sampling profile choices. <c>Auto</c> defers to the plugin's AICIMO decision.</summary>
public enum CnetHarnessSamplingMode : uint
{
    Auto = 0,
    Deterministic = 1,
    Focused = 2,
    Balanced = 3,
    Exploratory = 4,
}

/// <summary>Named single-bit resource selectors. Pass exactly one of these
/// as <see cref="CnetHarnessConfig.ResourceMask"/> — the native ABI is a
/// <c>ulong</c>, but this enum keeps callers from writing magic bit masks
/// by hand.</summary>
public enum CnetHarnessResource : ulong
{
    Cpu  = 1ul << 0,
    Gpu0 = 1ul << 1,
    Gpu1 = 1ul << 2,
    Gpu2 = 1ul << 3,
    Gpu3 = 1ul << 4,
}

/// <summary>Session-open configuration. All fields required unless noted.</summary>
public sealed class CnetHarnessConfig
{
    public string ModelId { get; init; } = string.Empty;
    public string ModelPath { get; init; } = string.Empty;
    /// <summary>ABI-preserving raw mask. Prefer <see cref="Resource"/>.</summary>
    public ulong ResourceMask { get; init; }
    /// <summary>Named projection. Setting this overrides <see cref="ResourceMask"/>.</summary>
    public CnetHarnessResource Resource
    {
        init => ResourceMask = (ulong)value;
    }
    public ulong BudgetBytes { get; init; }
    /// <summary>llama.cpp main_gpu index; -1 means "backend picks / not applicable" (used for CPU).</summary>
    public int MainGpu { get; init; } = -1;
    public uint ContextTokens { get; init; }
    public uint BatchTokens { get; init; }
    public uint Threads { get; init; }
    public uint AicimoNumOps { get; init; } = 4;
    public uint AicimoBaseDim { get; init; } = 32;
}

/// <summary>Per-request generation options. <see cref="Role"/> feeds AICIMO.</summary>
public sealed class CnetHarnessGenerateOptions
{
    public string? System { get; init; }
    public string User { get; init; } = string.Empty;
    public string Role { get; init; } = string.Empty;
    public uint MaxTokens { get; init; } = 128;
    public uint Seed { get; init; } = 424242;
    public CnetHarnessSamplingMode Sampling { get; init; } = CnetHarnessSamplingMode.Auto;
}

/// <summary>Result of a single generation call.</summary>
public sealed record CnetHarnessGenerationResult(
    string Text,
    uint PromptTokens,
    uint GeneratedTokens,
    double PromptMs,
    double GenerationMs,
    uint SelectedAdapter,
    float RouteUncertainty,
    CnetHarnessSamplingMode EffectiveSampling,
    bool AicimoOverride,
    float EffectiveTemperature,
    float EffectiveTopP,
    uint EffectiveTopK,
    float EffectiveMinP);

/// <summary>Non-generative route probe result — the same metadata generate() applies.</summary>
public sealed record CnetHarnessRouteInfo(
    uint SelectedAdapter,
    float RouteUncertainty,
    CnetHarnessSamplingMode EffectiveSampling,
    float EffectiveTemperature,
    float EffectiveTopP,
    uint EffectiveTopK,
    float EffectiveMinP);

/// <summary>Thrown when the native plugin reports a non-OK status.</summary>
public sealed class CnetHarnessException : Exception
{
    public CnetHarnessStatus Status { get; }

    public CnetHarnessException(CnetHarnessStatus status, string message)
        : base(message)
    {
        Status = status;
    }
}
