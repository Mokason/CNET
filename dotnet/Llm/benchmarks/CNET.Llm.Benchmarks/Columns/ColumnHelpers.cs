using BenchmarkDotNet.Running;

namespace CNET.Llm.Benchmarks.Columns;

/// <summary>
/// Shared helpers for custom inference metric columns.
/// </summary>
internal static class ColumnHelpers
{
    /// <summary>
    /// Extracts the <see cref="BenchmarkModel"/> parameter from a benchmark case, if present.
    /// </summary>
    public static BenchmarkModel? TryGetModel(BenchmarkCase benchmarkCase)
    {
        if (!benchmarkCase.HasParameters)
            return null;

        var items = benchmarkCase.Parameters.Items;
        foreach (var item in items)
        {
            if (item.Value is BenchmarkModel model)
                return model;
        }

        return null;
    }

    /// <summary>
    /// Returns the metrics key for a benchmark case. When <c>CNET_LLM_BENCH_MODEL_PATH</c> is set,
    /// uses the filename stem; otherwise falls back to the <see cref="BenchmarkModel"/> enum name.
    /// </summary>
    public static string? TryGetMetricsKey(BenchmarkCase benchmarkCase)
    {
        var envPath = Environment.GetEnvironmentVariable("CNET_LLM_BENCH_MODEL_PATH");
        if (!string.IsNullOrEmpty(envPath))
            return Path.GetFileNameWithoutExtension(envPath);

        var model = TryGetModel(benchmarkCase);
        return model?.ToString();
    }
}
