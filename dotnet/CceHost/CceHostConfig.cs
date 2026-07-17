using System;
using System.Globalization;
using CNET.Cce.CnetHarness;

namespace CNET.CceHost;

/// <summary>
/// Environment-driven configuration for the CNET .NET host. Defaults are CPU
/// + MainGpu = -1 so the native harness works without any GPU present. All
/// env values are parsed strictly: invalid values throw
/// <see cref="ArgumentException"/> with a specific message so misconfiguration
/// never silently succeeds.
/// </summary>
public sealed record CceHostConfig(
    CnetHarnessResource Resource,
    int MainGpu,
    ulong BudgetBytes,
    uint ContextTokens,
    uint BatchTokens,
    uint Threads)
{
    public const string EnvResourceMask = "CNET_HARNESS_RESOURCE_MASK";
    public const string EnvMainGpu = "CNET_HARNESS_MAIN_GPU";
    public const string EnvBudgetBytes = "CNET_HARNESS_BUDGET_BYTES";
    public const string EnvContextTokens = "CNET_HARNESS_CONTEXT_TOKENS";
    public const string EnvBatchTokens = "CNET_HARNESS_BATCH_TOKENS";
    public const string EnvThreads = "CNET_HARNESS_THREADS";

    public static CceHostConfig FromEnvironment() =>
        FromEnvironment(Environment.GetEnvironmentVariable);

    /// <summary>Testable overload — inject an environment lookup.</summary>
    public static CceHostConfig FromEnvironment(Func<string, string?> getEnv)
    {
        CnetHarnessResource resource = ParseResource(
            getEnv(EnvResourceMask), CnetHarnessResource.Cpu);
        int mainGpu = ParseInt(getEnv(EnvMainGpu), -1);
        ulong budget = ParseUlong(getEnv(EnvBudgetBytes),
            4ul * 1024ul * 1024ul * 1024ul);
        uint ctx = ParseUint(getEnv(EnvContextTokens), 4096u);
        uint batch = ParseUint(getEnv(EnvBatchTokens), Math.Min(4096u, ctx));
        uint threads = ParseUint(getEnv(EnvThreads), 8u);
        if (mainGpu < -1)
            throw new ArgumentException($"{EnvMainGpu} must be >= -1; got {mainGpu}");
        if (ctx < 256u)
            throw new ArgumentException($"{EnvContextTokens} must be >= 256; got {ctx}");
        if (batch > ctx)
        {
            throw new ArgumentException(
                $"{EnvBatchTokens}={batch} must be <= {EnvContextTokens}={ctx}");
        }
        if (threads > 1024u)
            throw new ArgumentException($"{EnvThreads} must be <= 1024; got {threads}");
        return new CceHostConfig(resource, mainGpu, budget, ctx, batch, threads);
    }

    private static CnetHarnessResource ParseResource(string? raw, CnetHarnessResource fallback)
    {
        if (string.IsNullOrEmpty(raw)) return fallback;
        switch (raw.Trim().ToLowerInvariant())
        {
            case "cpu":  return CnetHarnessResource.Cpu;
            case "gpu0": return CnetHarnessResource.Gpu0;
            case "gpu1": return CnetHarnessResource.Gpu1;
            case "gpu2": return CnetHarnessResource.Gpu2;
            case "gpu3": return CnetHarnessResource.Gpu3;
        }
        string trimmed = raw.Trim();
        if (trimmed.StartsWith("0x", StringComparison.OrdinalIgnoreCase) &&
            ulong.TryParse(trimmed.AsSpan(2), NumberStyles.AllowHexSpecifier,
                CultureInfo.InvariantCulture, out ulong hex) && IsSingleBitKnown(hex))
        {
            return (CnetHarnessResource)hex;
        }
        if (ulong.TryParse(trimmed, NumberStyles.Integer, CultureInfo.InvariantCulture,
                out ulong dec) && IsSingleBitKnown(dec))
        {
            return (CnetHarnessResource)dec;
        }
        throw new ArgumentException(
            $"{EnvResourceMask} must be cpu|gpu0|gpu1|gpu2|gpu3 (or matching single-bit mask); got '{raw}'.");
    }

    private static bool IsSingleBitKnown(ulong m) =>
        m != 0ul && (m & (m - 1ul)) == 0ul &&
        (m & (ulong)(CnetHarnessResource.Cpu | CnetHarnessResource.Gpu0 |
                     CnetHarnessResource.Gpu1 | CnetHarnessResource.Gpu2 |
                     CnetHarnessResource.Gpu3)) == m;

    private static int ParseInt(string? raw, int fallback)
    {
        if (string.IsNullOrEmpty(raw)) return fallback;
        if (int.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture,
                out int v)) return v;
        throw new ArgumentException($"integer env var could not be parsed: '{raw}'");
    }

    private static uint ParseUint(string? raw, uint fallback)
    {
        if (string.IsNullOrEmpty(raw)) return fallback;
        if (uint.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture,
                out uint v) && v > 0u) return v;
        throw new ArgumentException($"positive uint env var could not be parsed: '{raw}'");
    }

    private static ulong ParseUlong(string? raw, ulong fallback)
    {
        if (string.IsNullOrEmpty(raw)) return fallback;
        if (ulong.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture,
                out ulong v) && v > 0ul) return v;
        throw new ArgumentException($"positive ulong env var could not be parsed: '{raw}'");
    }
}
