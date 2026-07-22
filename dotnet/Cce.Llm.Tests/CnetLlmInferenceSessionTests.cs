using CNET.Cce.CnetHarness;
using CNET.Cce.Llm;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// Locates a GGUF fixture. The integration fixtures the CNET.Llm suite already
/// downloads are reused rather than pulling another copy.
/// </summary>
internal static class TestModel
{
    public static readonly string? Path = Resolve();

    private static string? Resolve()
    {
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        string[] candidates =
        [
            System.IO.Path.Combine(home, ".cnet-llm", "test-cache", "QuantFactory",
                "SmolLM-135M-GGUF", "SmolLM-135M.Q8_0.gguf"),
            System.IO.Path.Combine(home, ".cnet-llm", "test-cache", "QuantFactory",
                "SmolLM-135M-GGUF", "SmolLM-135M.Q4_K_M.gguf"),
        ];
        return Array.Find(candidates, File.Exists);
    }
}

/// <summary>Fact that skips when no GGUF fixture is available locally.</summary>
public sealed class ModelFactAttribute : FactAttribute
{
    public ModelFactAttribute()
    {
        if (TestModel.Path is null)
            Skip = "no local GGUF fixture; run the CNET.Llm integration tests first";
    }
}

public class SamplingProfileTests
{
    [Fact]
    public void Deterministic_IsGreedy()
    {
        var p = CnetLlmSamplingProfile.Resolve(CnetHarnessSamplingMode.Deterministic);
        Assert.Equal(CnetHarnessSamplingMode.Deterministic, p.Mode);
        Assert.Equal(0.0f, p.Temperature);
        Assert.Equal(0u, p.TopK);
    }

    [Fact]
    public void Auto_ResolvesToBalanced_AndReportsBalanced()
    {
        // There is no AICIMO in the managed backend, so Auto must not be
        // reported back as Auto — callers reading EffectiveSampling need to see
        // what was actually applied.
        var p = CnetLlmSamplingProfile.Resolve(CnetHarnessSamplingMode.Auto);
        Assert.Equal(CnetHarnessSamplingMode.Balanced, p.Mode);
        Assert.Equal(CnetLlmSamplingProfile.Resolve(CnetHarnessSamplingMode.Balanced), p);
    }

    [Theory]
    [InlineData(CnetHarnessSamplingMode.Focused)]
    [InlineData(CnetHarnessSamplingMode.Balanced)]
    [InlineData(CnetHarnessSamplingMode.Exploratory)]
    public void StochasticProfiles_HaveSaneRanges(CnetHarnessSamplingMode mode)
    {
        var p = CnetLlmSamplingProfile.Resolve(mode);
        Assert.Equal(mode, p.Mode);
        Assert.InRange(p.Temperature, 0.01f, 2.0f);
        Assert.InRange(p.TopP, 0.0f, 1.0f);
        Assert.InRange(p.MinP, 0.0f, 1.0f);
        Assert.True(p.TopK > 0);
    }

    [Fact]
    public void Profiles_OrderedByIncreasingTemperature()
    {
        float focused = CnetLlmSamplingProfile.Resolve(CnetHarnessSamplingMode.Focused).Temperature;
        float balanced = CnetLlmSamplingProfile.Resolve(CnetHarnessSamplingMode.Balanced).Temperature;
        float exploratory = CnetLlmSamplingProfile.Resolve(CnetHarnessSamplingMode.Exploratory).Temperature;
        Assert.True(focused < balanced && balanced < exploratory);
    }
}

public class OpenValidationTests
{
    private static CnetHarnessConfig BaseConfig(string modelPath) => new()
    {
        ModelId = "test",
        ModelPath = modelPath,
        Resource = CnetHarnessResource.Cpu,
        BudgetBytes = 1UL << 30,
        ContextTokens = 512,
        BatchTokens = 512,
        Threads = 4,
    };

    [Fact]
    public void NullConfig_Throws()
        => Assert.Throws<ArgumentNullException>(() => CnetLlmInferenceSession.Open(null!));

    [Fact]
    public void MissingModelFile_ReportsModelLoadFailed()
    {
        var ex = Assert.Throws<CnetHarnessException>(
            () => CnetLlmInferenceSession.Open(BaseConfig("/nonexistent/model.gguf")));
        Assert.Equal(CnetHarnessStatus.ModelLoadFailed, ex.Status);
    }

    [Fact]
    public void GpuResource_IsRejected_NotSilentlyDowngradedToCpu()
    {
        var config = new CnetHarnessConfig
        {
            ModelId = "test",
            ModelPath = "/nonexistent/model.gguf",
            Resource = CnetHarnessResource.Gpu0,
            BudgetBytes = 1UL << 30,
            ContextTokens = 512,
            BatchTokens = 512,
            Threads = 4,
        };
        var ex = Assert.Throws<CnetHarnessException>(() => CnetLlmInferenceSession.Open(config));
        Assert.Equal(CnetHarnessStatus.InvalidArgument, ex.Status);
        Assert.Contains("CPU only", ex.Message);
    }

    [Fact]
    public void GpuOffload_IsRejected()
    {
        var config = new CnetHarnessConfig
        {
            ModelId = "test",
            ModelPath = "/nonexistent/model.gguf",
            Resource = CnetHarnessResource.Gpu0,
            BudgetBytes = 1UL << 30,
            ContextTokens = 512,
            BatchTokens = 512,
            Threads = 4,
            GpuOffload = new CnetHarnessGpuOffload { LayerCount = 4, DeviceIndices = new[] { 0 } },
        };
        var ex = Assert.Throws<CnetHarnessException>(() => CnetLlmInferenceSession.Open(config));
        Assert.Equal(CnetHarnessStatus.InvalidArgument, ex.Status);
    }

    [Theory]
    [InlineData(0u, 512u)]    // ContextTokens below the 256 floor
    [InlineData(512u, 0u)]    // BatchTokens must be in (0, ContextTokens]
    public void SharedValidatorRejectsBadShapes(uint contextTokens, uint batchTokens)
    {
        // Proves the bridge routes through CnetHarnessSession's validators
        // rather than re-implementing (and drifting from) them.
        var config = new CnetHarnessConfig
        {
            ModelId = "test",
            ModelPath = "/nonexistent/model.gguf",
            Resource = CnetHarnessResource.Cpu,
            BudgetBytes = 1UL << 30,
            ContextTokens = contextTokens,
            BatchTokens = batchTokens,
            Threads = 4,
        };
        var ex = Assert.Throws<CnetHarnessException>(() => CnetLlmInferenceSession.Open(config));
        Assert.Equal(CnetHarnessStatus.InvalidArgument, ex.Status);
    }
}

public class ManagedGenerationTests
{
    private static CnetHarnessConfig Config() => new()
    {
        ModelId = "smollm-135m",
        ModelPath = TestModel.Path!,
        Resource = CnetHarnessResource.Cpu,
        BudgetBytes = 2UL << 30,
        ContextTokens = 512,
        BatchTokens = 512,
        Threads = 8,
    };

    [ModelFact]
    public void Generate_ProducesTokens_AndReportsTimings()
    {
        using var session = CnetLlmInferenceSession.Open(Config());
        var result = session.Generate(new CnetHarnessGenerateOptions
        {
            User = "The capital of France is",
            Role = "test",
            MaxTokens = 16,
            Sampling = CnetHarnessSamplingMode.Deterministic,
        });

        Assert.False(string.IsNullOrWhiteSpace(result.Text));
        Assert.True(result.GeneratedTokens > 0);
        Assert.True(result.PromptTokens > 0);
        Assert.True(result.GenerationMs > 0);
        Assert.Equal(CnetHarnessSamplingMode.Deterministic, result.EffectiveSampling);
        Assert.False(result.AicimoOverride);
    }

    [ModelFact]
    public void Deterministic_IsReproducible()
    {
        using var session = CnetLlmInferenceSession.Open(Config());
        var options = new CnetHarnessGenerateOptions
        {
            User = "The capital of France is",
            Role = "test",
            MaxTokens = 16,
            Seed = 424242,
            Sampling = CnetHarnessSamplingMode.Deterministic,
        };
        Assert.Equal(session.Generate(options).Text, session.Generate(options).Text);
    }

    [ModelFact]
    public void ProbeRoute_AgreesWithWhatGenerateApplies()
    {
        using var session = CnetLlmInferenceSession.Open(Config());
        var route = session.ProbeRoute("test", CnetHarnessSamplingMode.Focused);
        var result = session.Generate(new CnetHarnessGenerateOptions
        {
            User = "hello",
            Role = "test",
            MaxTokens = 4,
            Sampling = CnetHarnessSamplingMode.Focused,
        });

        Assert.Equal(route.EffectiveSampling, result.EffectiveSampling);
        Assert.Equal(route.EffectiveTemperature, result.EffectiveTemperature);
        Assert.Equal(route.EffectiveTopP, result.EffectiveTopP);
        Assert.Equal(route.EffectiveTopK, result.EffectiveTopK);
        Assert.Equal(route.EffectiveMinP, result.EffectiveMinP);
    }

    [ModelFact]
    public void UsableThroughTheBackendAgnosticInterface()
    {
        // The whole point of the wiring: a host holds ICnetInferenceSession and
        // does not know or care which backend it got.
        using ICnetInferenceSession session = CnetLlmInferenceSession.Open(Config());
        var result = session.Generate(new CnetHarnessGenerateOptions
        {
            User = "hello",
            Role = "test",
            MaxTokens = 4,
            Sampling = CnetHarnessSamplingMode.Deterministic,
        });
        Assert.False(string.IsNullOrEmpty(result.Text));
    }

    [ModelFact]
    public void DisposeIsIdempotent_AndUseAfterDisposeThrows()
    {
        var session = CnetLlmInferenceSession.Open(Config());
        session.Dispose();
        session.Dispose();
        Assert.Throws<ObjectDisposedException>(() => session.Generate(
            new CnetHarnessGenerateOptions { User = "x", Role = "test", MaxTokens = 4 }));
    }

    [ModelFact]
    public void InvalidGenerateOptions_ReportInvalidArgument()
    {
        using var session = CnetLlmInferenceSession.Open(Config());
        var ex = Assert.Throws<CnetHarnessException>(() => session.Generate(
            new CnetHarnessGenerateOptions { User = "", Role = "test", MaxTokens = 4 }));
        Assert.Equal(CnetHarnessStatus.InvalidArgument, ex.Status);
    }
}

public class ContractParityTests
{
    [Fact]
    public void NativeSessionAlsoImplementsTheInterface()
    {
        // Compile-time proof that both backends satisfy one contract; asserting
        // on the type avoids needing cnet.so present to run this.
        Assert.True(typeof(ICnetInferenceSession)
            .IsAssignableFrom(typeof(CnetHarnessSession)));
        Assert.True(typeof(ICnetInferenceSession)
            .IsAssignableFrom(typeof(CnetLlmInferenceSession)));
    }
}
