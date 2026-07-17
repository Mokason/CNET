using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using CNET.Cce.CnetHarness;
using CNET.CceHost;
using Xunit;

namespace CNET.Cce.Tests;

/// <summary>
/// Managed unit tests for the CNET .NET harness. Uses a fake
/// <see cref="ICnetHarnessNative"/> to exercise the managed contracts without
/// loading libcnet_harness.so or a real GGUF.
/// </summary>
public sealed class CnetHarnessTests
{
    private static CnetHarnessConfig MakeConfig() => new()
    {
        ModelId = "unit",
        ModelPath = "/tmp/unit.gguf",
        ResourceMask = 1ul << 2,
        BudgetBytes = 4ul * 1024 * 1024 * 1024,
        MainGpu = 0,
        ContextTokens = 2048,
        BatchTokens = 512,
        Threads = 4,
        AicimoNumOps = 4,
        AicimoBaseDim = 32,
    };

    // ---- option validation ----

    [Fact]
    public void Open_NullConfig_Throws()
    {
        var fake = new FakeNative();
        Assert.Throws<ArgumentNullException>(() =>
            CnetHarnessSession.Open(null!, fake));
    }

    [Fact]
    public void Open_EmptyModelId_Throws()
    {
        var fake = new FakeNative();
        var baseCfg = MakeConfig();
        var c = new CnetHarnessConfig
        {
            ModelId = "",
            ModelPath = baseCfg.ModelPath,
            ResourceMask = baseCfg.ResourceMask,
            BudgetBytes = baseCfg.BudgetBytes,
            MainGpu = baseCfg.MainGpu,
            ContextTokens = baseCfg.ContextTokens,
            BatchTokens = baseCfg.BatchTokens,
            Threads = baseCfg.Threads,
            AicimoNumOps = baseCfg.AicimoNumOps,
            AicimoBaseDim = baseCfg.AicimoBaseDim,
        };
        Assert.Throws<ArgumentException>(() => CnetHarnessSession.Open(c, fake));
    }

    [Fact]
    public void Open_ZeroResourceMask_Throws()
    {
        var fake = new FakeNative();
        var c = new CnetHarnessConfig
        {
            ModelId = "unit",
            ModelPath = "/tmp/unit.gguf",
            ResourceMask = 0,
            BudgetBytes = 1,
            MainGpu = 0,
            ContextTokens = 2048,
            BatchTokens = 512,
            Threads = 4,
        };
        Assert.Throws<ArgumentException>(() => CnetHarnessSession.Open(c, fake));
    }

    [Fact]
    public void Open_TooSmallAicimoDim_Throws()
    {
        var fake = new FakeNative();
        var c = new CnetHarnessConfig
        {
            ModelId = "unit",
            ModelPath = "/tmp/unit.gguf",
            ResourceMask = 1,
            BudgetBytes = 1,
            MainGpu = 0,
            ContextTokens = 2048,
            BatchTokens = 512,
            Threads = 4,
            AicimoNumOps = 3,     // < 4
            AicimoBaseDim = 32,
        };
        Assert.Throws<ArgumentException>(() => CnetHarnessSession.Open(c, fake));
    }

    // ---- native failure propagation ----

    [Fact]
    public void Open_NativeReturnsError_ThrowsCnetHarnessException()
    {
        var fake = new FakeNative
        {
            OpenReturn = (int)CnetHarnessStatus.ModelLoadFailed,
        };
        var ex = Assert.Throws<CnetHarnessException>(
            () => CnetHarnessSession.Open(MakeConfig(), fake));
        Assert.Equal(CnetHarnessStatus.ModelLoadFailed, ex.Status);
        Assert.Contains("model load failed", ex.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void Generate_NativeReturnsError_ThrowsAndFreesNothing()
    {
        var fake = new FakeNative { OpenReturn = 0 };
        using var s = CnetHarnessSession.Open(MakeConfig(), fake);
        fake.GenerateReturn = (int)CnetHarnessStatus.BackendFailure;
        var options = new CnetHarnessGenerateOptions
        {
            User = "hi", Role = "planner", MaxTokens = 8,
        };
        Assert.Throws<CnetHarnessException>(() => s.Generate(options));
        Assert.Equal(0, fake.GenerationFreeCalls); // nothing to free
    }

    // ---- UTF-8 projection and deterministic single free ----

    [Fact]
    public void Generate_ReturnsUtf8Text_AndFreesGenerationExactlyOnce()
    {
        var fake = new FakeNative { OpenReturn = 0 };
        using var s = CnetHarnessSession.Open(MakeConfig(), fake);

        const string emoji = "Hello 世界 🌟";  /* "Hello 世界 🌟" */
        fake.GenerationText = emoji;
        fake.GenerationPromptTokens = 5;
        fake.GenerationGeneratedTokens = 7;
        fake.GenerationSelectedAdapter = 2;
        fake.GenerationRouteUncertainty = 0.42f;
        fake.GenerationEffectiveSampling = CnetHarnessSamplingMode.Balanced;
        fake.GenerationAicimoOverride = false;

        var options = new CnetHarnessGenerateOptions
        {
            User = "hi", Role = "planner", MaxTokens = 8,
        };
        var result = s.Generate(options);

        Assert.Equal(emoji, result.Text);
        Assert.Equal((uint)5, result.PromptTokens);
        Assert.Equal((uint)7, result.GeneratedTokens);
        Assert.Equal((uint)2, result.SelectedAdapter);
        Assert.Equal(0.42f, result.RouteUncertainty);
        Assert.Equal(CnetHarnessSamplingMode.Balanced, result.EffectiveSampling);
        Assert.False(result.AicimoOverride);

        Assert.Equal(1, fake.GenerationFreeCalls);
    }

    [Fact]
    public void Dispose_ClosesSessionExactlyOnce()
    {
        var fake = new FakeNative { OpenReturn = 0 };
        var s = CnetHarnessSession.Open(MakeConfig(), fake);
        s.Dispose();
        s.Dispose();  /* second dispose must be a no-op */
        Assert.Equal(1, fake.CloseCalls);
    }

    [Fact]
    public void Generate_AfterDispose_Throws()
    {
        var fake = new FakeNative { OpenReturn = 0 };
        var s = CnetHarnessSession.Open(MakeConfig(), fake);
        s.Dispose();
        var options = new CnetHarnessGenerateOptions
        {
            User = "hi", Role = "planner", MaxTokens = 8,
        };
        Assert.Throws<ObjectDisposedException>(() => s.Generate(options));
    }

    // ---- ProbeRoute + fake role behavior ----

    [Fact]
    public void ProbeRoute_ReturnsMetadata_FromNative()
    {
        var fake = new FakeNative
        {
            OpenReturn = 0,
            ProbeRouteImpl = (IntPtr _, string role, CnetHarnessSamplingMode over,
                             ref NativeRouteInfo info) =>
            {
                info.SelectedAdapter = (uint)(role.Length % 4);
                info.RouteUncertainty = 0.5f;
                info.EffectiveSampling = over == CnetHarnessSamplingMode.Auto
                    ? CnetHarnessSamplingMode.Focused
                    : over;
                return 0;
            }
        };
        using var s = CnetHarnessSession.Open(MakeConfig(), fake);

        var info = s.ProbeRoute("analytical");
        Assert.Equal((uint)("analytical".Length % 4), info.SelectedAdapter);
        Assert.Equal(CnetHarnessSamplingMode.Focused, info.EffectiveSampling);

        var overridden = s.ProbeRoute("analytical", CnetHarnessSamplingMode.Deterministic);
        Assert.Equal(CnetHarnessSamplingMode.Deterministic, overridden.EffectiveSampling);
    }

    // ---- IChatClient / CceHost abstraction ----

    [Fact]
    public async System.Threading.Tasks.Task CnetHarnessChatClient_ImplementsIChatClient_AndConcatenatesTurns()
    {
        var fake = new FakeNative { OpenReturn = 0 };
        using var s = CnetHarnessSession.Open(MakeConfig(), fake);
        fake.GenerationText = "assistant reply";

        using var chat = new CnetHarnessChatClient(s, role: "planner",
                                                    maxTokens: 8, ownsSession: false);
        IChatClient client = chat;
        var reply = await client.ChatAsync(new (string, string)[]
        {
            ("system", "sys-instr"),
            ("user", "hello"),
            ("assistant", "hi back"),
            ("user", "another"),
        });

        Assert.Equal("assistant reply", reply);
        Assert.Contains("sys-instr", fake.LastSystem);
        Assert.Contains("user: hello", fake.LastUser);
        Assert.Contains("assistant: hi back", fake.LastUser);
        Assert.Contains("user: another", fake.LastUser);
        Assert.Equal("planner", fake.LastRole);
    }

    // ---- fake native invoker ----

    private sealed class FakeNative : ICnetHarnessNative
    {
        public int OpenReturn = 0;
        public int GenerateReturn = 0;
        public string GenerationText = "";
        public uint GenerationPromptTokens = 0;
        public uint GenerationGeneratedTokens = 0;
        public uint GenerationSelectedAdapter = 0;
        public float GenerationRouteUncertainty = 0f;
        public CnetHarnessSamplingMode GenerationEffectiveSampling
            = CnetHarnessSamplingMode.Deterministic;
        public bool GenerationAicimoOverride = false;
        public int GenerationFreeCalls = 0;
        public int CloseCalls = 0;
        public string LastSystem = "";
        public string LastUser = "";
        public string LastRole = "";

        public delegate int ProbeRouteHandler(IntPtr session, string role,
            CnetHarnessSamplingMode overrideMode, ref NativeRouteInfo info);
        public ProbeRouteHandler? ProbeRouteImpl;

        private const int SessionSentinel = 0x1234;

        public int Open(in NativeConfig config, out IntPtr session)
        {
            if (OpenReturn != 0)
            {
                session = IntPtr.Zero;
                return OpenReturn;
            }
            session = new IntPtr(SessionSentinel);
            return 0;
        }

        public int Generate(IntPtr session, in NativeGenerateOptions options,
                            out IntPtr generation)
        {
            LastRole = options.Role == IntPtr.Zero ? ""
                : Marshal.PtrToStringUTF8(options.Role) ?? "";
            LastUser = options.User == IntPtr.Zero ? ""
                : Marshal.PtrToStringUTF8(options.User) ?? "";
            LastSystem = options.System == IntPtr.Zero ? ""
                : Marshal.PtrToStringUTF8(options.System) ?? "";

            if (GenerateReturn != 0)
            {
                generation = IntPtr.Zero;
                return GenerateReturn;
            }
            /* Return a non-zero sentinel; ReadGeneration will look it up. */
            generation = new IntPtr(0x5678);
            return 0;
        }

        public int ProbeRoute(IntPtr session, string role,
                              CnetHarnessSamplingMode overrideMode,
                              ref NativeRouteInfo info)
        {
            if (ProbeRouteImpl != null)
                return ProbeRouteImpl(session, role, overrideMode, ref info);
            info.SelectedAdapter = 1;
            info.RouteUncertainty = 0.1f;
            info.EffectiveSampling = overrideMode == CnetHarnessSamplingMode.Auto
                ? CnetHarnessSamplingMode.Focused
                : overrideMode;
            return 0;
        }

        public NativeGenerationLayout ReadGeneration(IntPtr generation)
        {
            return new NativeGenerationLayout(
                GenerationText,
                GenerationPromptTokens,
                GenerationGeneratedTokens,
                12.5,
                42.0,
                GenerationSelectedAdapter,
                GenerationRouteUncertainty,
                GenerationEffectiveSampling,
                GenerationAicimoOverride);
        }

        public void GenerationFree(IntPtr generation)
        {
            if (generation != IntPtr.Zero) GenerationFreeCalls++;
        }

        public int Close(IntPtr session)
        {
            CloseCalls++;
            return 0;
        }

        public string ErrorString(int status) => status switch
        {
            0  => "ok",
            -1 => "invalid argument",
            -2 => "model load failed",
            -3 => "backend failure",
            -4 => "invalid session state",
            -5 => "internal error",
            _  => "unknown",
        };
    }
}
