using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
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
        Resource = CnetHarnessResource.Gpu1,
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
    public void Open_MultiBitResourceMask_Throws()
    {
        var fake = new FakeNative();
        var c = new CnetHarnessConfig
        {
            ModelId = "unit",
            ModelPath = "/tmp/unit.gguf",
            ResourceMask = (ulong)(CnetHarnessResource.Gpu0 | CnetHarnessResource.Gpu1),
            BudgetBytes = 1,
            MainGpu = 0,
            ContextTokens = 2048,
            BatchTokens = 512,
            Threads = 4,
        };
        Assert.Throws<ArgumentException>(() => CnetHarnessSession.Open(c, fake));
    }

    [Fact]
    public void Open_UnknownResourceBit_Throws()
    {
        var fake = new FakeNative();
        var c = new CnetHarnessConfig
        {
            ModelId = "unit",
            ModelPath = "/tmp/unit.gguf",
            ResourceMask = 1ul << 20,  /* not a known resource */
            BudgetBytes = 1,
            MainGpu = 0,
            ContextTokens = 2048,
            BatchTokens = 512,
            Threads = 4,
        };
        Assert.Throws<ArgumentException>(() => CnetHarnessSession.Open(c, fake));
    }

    [Fact]
    public void Open_ZeroBudget_Throws()
    {
        var fake = new FakeNative();
        var c = new CnetHarnessConfig
        {
            ModelId = "unit",
            ModelPath = "/tmp/unit.gguf",
            Resource = CnetHarnessResource.Cpu,
            BudgetBytes = 0,
            MainGpu = -1,
            ContextTokens = 2048,
            BatchTokens = 512,
            Threads = 4,
        };
        Assert.Throws<ArgumentException>(() => CnetHarnessSession.Open(c, fake));
    }

    [Fact]
    public void Open_CpuResource_Accepted()
    {
        var fake = new FakeNative { OpenReturn = 0 };
        var c = new CnetHarnessConfig
        {
            ModelId = "unit",
            ModelPath = "/tmp/unit.gguf",
            Resource = CnetHarnessResource.Cpu,
            BudgetBytes = 1,
            MainGpu = -1,
            ContextTokens = 2048,
            BatchTokens = 512,
            Threads = 4,
        };
        using var s = CnetHarnessSession.Open(c, fake);
        Assert.NotNull(s);
    }

    [Fact]
    public void Open_TooSmallAicimoDim_Throws()
    {
        var fake = new FakeNative();
        var c = new CnetHarnessConfig
        {
            ModelId = "unit",
            ModelPath = "/tmp/unit.gguf",
            Resource = CnetHarnessResource.Cpu,
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

    // ---- effective sampling parameter projection ----

    [Fact]
    public void Generate_ProjectsEffectiveSamplingParams_FromNative()
    {
        var fake = new FakeNative { OpenReturn = 0 };
        using var s = CnetHarnessSession.Open(MakeConfig(), fake);
        fake.GenerationText = "ok";
        fake.GenerationEffectiveSampling = CnetHarnessSamplingMode.Focused;
        fake.GenerationEffectiveTemperature = 0.30f;
        fake.GenerationEffectiveTopP = 0.85f;
        fake.GenerationEffectiveTopK = 40u;
        fake.GenerationEffectiveMinP = 0.05f;

        var result = s.Generate(new CnetHarnessGenerateOptions
        {
            User = "hi", Role = "planner", MaxTokens = 4,
        });

        Assert.Equal(0.30f, result.EffectiveTemperature);
        Assert.Equal(0.85f, result.EffectiveTopP);
        Assert.Equal(40u, result.EffectiveTopK);
        Assert.Equal(0.05f, result.EffectiveMinP);
    }

    [Fact]
    public void Generate_DifferentProfiles_HaveDifferentEffectiveParams()
    {
        var fake = new FakeNative { OpenReturn = 0 };
        using var s = CnetHarnessSession.Open(MakeConfig(), fake);
        fake.GenerationText = "ok";

        fake.GenerationEffectiveSampling = CnetHarnessSamplingMode.Deterministic;
        fake.GenerationEffectiveTemperature = 0.0f;
        fake.GenerationEffectiveTopP = 1.0f;
        fake.GenerationEffectiveTopK = 0u;
        fake.GenerationEffectiveMinP = 0.0f;
        var det = s.Generate(new CnetHarnessGenerateOptions
        {
            User = "hi", Role = "planner", MaxTokens = 4,
        });

        fake.GenerationEffectiveSampling = CnetHarnessSamplingMode.Exploratory;
        fake.GenerationEffectiveTemperature = 0.95f;
        fake.GenerationEffectiveTopP = 0.95f;
        fake.GenerationEffectiveTopK = 80u;
        fake.GenerationEffectiveMinP = 0.03f;
        var exp = s.Generate(new CnetHarnessGenerateOptions
        {
            User = "hi", Role = "explorer", MaxTokens = 4,
        });

        Assert.NotEqual(det.EffectiveTemperature, exp.EffectiveTemperature);
        Assert.NotEqual(det.EffectiveTopP, exp.EffectiveTopP);
        Assert.NotEqual(det.EffectiveTopK, exp.EffectiveTopK);
    }

    [Fact]
    public async Task Generate_SerializesConcurrentCalls_OnOneSession()
    {
        using var entered = new ManualResetEventSlim(false);
        using var release = new ManualResetEventSlim(false);
        using var secondStarted = new ManualResetEventSlim(false);
        var fake = new FakeNative
        {
            OpenReturn = 0,
            GenerateEntered = entered,
            GenerateRelease = release,
        };
        using var s = CnetHarnessSession.Open(MakeConfig(), fake);
        var options = new CnetHarnessGenerateOptions
        {
            User = "hi", Role = "planner", MaxTokens = 4,
        };

        Task<CnetHarnessGenerationResult> first =
            Task.Run(() => s.Generate(options));
        Assert.True(entered.Wait(TimeSpan.FromSeconds(1)));
        Task<CnetHarnessGenerationResult> second = Task.Run(() =>
        {
            secondStarted.Set();
            return s.Generate(options);
        });
        Assert.True(secondStarted.Wait(TimeSpan.FromSeconds(1)));

        try
        {
            await Task.Delay(50);
            Assert.Equal(1, Volatile.Read(ref fake.MaxConcurrentGenerations));
        }
        finally
        {
            release.Set();
        }

        await Task.WhenAll(first, second);
        Assert.Equal(1, fake.MaxConcurrentGenerations);
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
                if (info.EffectiveSampling == CnetHarnessSamplingMode.Focused)
                {
                    info.EffectiveTemperature = 0.30f;
                    info.EffectiveTopP = 0.85f;
                    info.EffectiveTopK = 40u;
                    info.EffectiveMinP = 0.05f;
                }
                else
                {
                    info.EffectiveTemperature = 0.0f;
                    info.EffectiveTopP = 1.0f;
                    info.EffectiveTopK = 0u;
                    info.EffectiveMinP = 0.0f;
                }
                return 0;
            }
        };
        using var s = CnetHarnessSession.Open(MakeConfig(), fake);

        var info = s.ProbeRoute("analytical");
        Assert.Equal((uint)("analytical".Length % 4), info.SelectedAdapter);
        Assert.Equal(CnetHarnessSamplingMode.Focused, info.EffectiveSampling);
        Assert.Equal(0.30f, info.EffectiveTemperature);
        Assert.Equal(40u, info.EffectiveTopK);

        var overridden = s.ProbeRoute("analytical", CnetHarnessSamplingMode.Deterministic);
        Assert.Equal(CnetHarnessSamplingMode.Deterministic, overridden.EffectiveSampling);
        Assert.Equal(0.0f, overridden.EffectiveTemperature);
        Assert.Equal(1.0f, overridden.EffectiveTopP);
        Assert.Equal(0u, overridden.EffectiveTopK);
    }

    [Fact]
    public void ProbeRoute_InvalidSamplingOverride_ThrowsBeforeNativeCall()
    {
        var fake = new FakeNative { OpenReturn = 0 };
        using var s = CnetHarnessSession.Open(MakeConfig(), fake);
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            s.ProbeRoute("analytical", (CnetHarnessSamplingMode)999u));
    }

    [Fact]
    public void Generate_InvalidSamplingMode_ThrowsBeforeNativeCall()
    {
        var fake = new FakeNative { OpenReturn = 0 };
        using var s = CnetHarnessSession.Open(MakeConfig(), fake);
        Assert.Throws<ArgumentOutOfRangeException>(() => s.Generate(
            new CnetHarnessGenerateOptions
            {
                User = "hi", Role = "planner", MaxTokens = 4,
                Sampling = (CnetHarnessSamplingMode)999u,
            }));
    }

    // ---- IChatClient / CceHost abstraction ----

    [Fact]
    public async System.Threading.Tasks.Task CnetHarnessChatClient_ImplementsIChatClient_AndConcatenatesTurns()
    {
        var fake = new FakeNative { OpenReturn = 0 };
        using var s = CnetHarnessSession.Open(MakeConfig(), fake);
        fake.GenerationText = "assistant reply";

        using var chat = new CnetHarnessChatClient(s, role: "planner",
            maxTokens: 8, ownsSession: false,
            sampling: CnetHarnessSamplingMode.Deterministic, seed: 77);
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
        Assert.Equal(CnetHarnessSamplingMode.Deterministic, fake.LastSampling);
        Assert.Equal((uint)77, fake.LastSeed);
    }

    [Fact]
    public void CnetHarnessConversation_PublicBoundedMemoryContractExists()
    {
        Type? conversation = typeof(CnetHarnessChatClient).Assembly.GetType(
            "CNET.CceHost.CnetHarnessConversation");

        Assert.NotNull(conversation);
        Assert.NotNull(conversation!.GetConstructor(new[]
        {
            typeof(IChatClient), typeof(string), typeof(int), typeof(int), typeof(bool),
        }));
        Assert.NotNull(conversation.GetMethod("SendAsync", new[] { typeof(string) }));
        Assert.NotNull(conversation.GetMethod("Snapshot", Type.EmptyTypes));
        Assert.NotNull(conversation.GetProperty("RetainedTurnCount"));
        Assert.NotNull(conversation.GetProperty("RetainedCharacterCount"));

        Type[] deterministicClientSignature =
        {
            typeof(CnetHarnessSession), typeof(string), typeof(uint),
            typeof(bool), typeof(CnetHarnessSamplingMode), typeof(uint),
        };
        Assert.NotNull(typeof(CnetHarnessChatClient).GetConstructor(
            deterministicClientSignature));
    }

    [Fact]
    public async System.Threading.Tasks.Task CnetHarnessConversation_RetainsOnlyNewestCompleteTurns()
    {
        var fake = new RecordingChatClient("a1", "a2", "a3");
        using var conversation = new CnetHarnessConversation(
            fake, "system", maxRetainedTurns: 2,
            maxRetainedCharacters: 1024, ownsClient: false);

        Assert.Equal("a1", await conversation.SendAsync("u1"));
        Assert.Equal("a2", await conversation.SendAsync("u2"));
        Assert.Equal("a3", await conversation.SendAsync("u3"));

        Assert.Equal(2, conversation.RetainedTurnCount);
        Assert.Equal(new (string, string)[]
        {
            ("system", "system"),
            ("user", "u2"),
            ("assistant", "a2"),
            ("user", "u3"),
            ("assistant", "a3"),
        }, conversation.Snapshot());
        Assert.DoesNotContain(fake.Requests[2], m => m.Content == "u1");
        Assert.Contains(fake.Requests[2], m => m.Content == "u2");
        Assert.Contains(fake.Requests[2], m => m.Content == "u3");
        Assert.Equal(14, conversation.RetainedCharacterCount);
    }

    [Fact]
    public async System.Threading.Tasks.Task CnetHarnessConversation_CharacterBudgetEvictsOldestWholeTurn()
    {
        var fake = new RecordingChatClient("aaaa", "bbbb", "cccc");
        using var conversation = new CnetHarnessConversation(
            fake, "s", maxRetainedTurns: 4,
            maxRetainedCharacters: 17, ownsClient: false);

        await conversation.SendAsync("1111");
        await conversation.SendAsync("2222");
        await conversation.SendAsync("3333");

        Assert.Equal(2, conversation.RetainedTurnCount);
        Assert.Equal(17, conversation.RetainedCharacterCount);
        Assert.DoesNotContain(conversation.Snapshot(), m => m.Content == "1111");
        Assert.DoesNotContain(conversation.Snapshot(), m => m.Content == "aaaa");
    }

    [Fact]
    public async System.Threading.Tasks.Task CnetHarnessConversation_OversizedReplyIsReturnedButClippedInHistory()
    {
        const string fullReply = "abcdefghijklmnopqrst";
        var fake = new RecordingChatClient(fullReply);
        using var conversation = new CnetHarnessConversation(
            fake, "s", maxRetainedTurns: 2,
            maxRetainedCharacters: 10, ownsClient: false);

        Assert.Equal(fullReply, await conversation.SendAsync("1234"));

        Assert.Equal(new (string, string)[]
        {
            ("system", "s"),
            ("user", "1234"),
            ("assistant", "abcde"),
        }, conversation.Snapshot());
        Assert.Equal(1, conversation.RetainedTurnCount);
        Assert.Equal(10, conversation.RetainedCharacterCount);
    }

    [Fact]
    public async System.Threading.Tasks.Task CnetHarnessConversation_DisposeClearsHistoryAndOwnedClientExactlyOnce()
    {
        var fake = new RecordingChatClient("answer");
        var conversation = new CnetHarnessConversation(
            fake, "system", maxRetainedTurns: 2,
            maxRetainedCharacters: 128, ownsClient: true);
        await conversation.SendAsync("question");

        conversation.Dispose();
        conversation.Dispose();

        Assert.Equal(0, conversation.RetainedTurnCount);
        Assert.Equal(0, conversation.RetainedCharacterCount);
        Assert.Equal(1, fake.DisposeCalls);
        await Assert.ThrowsAsync<ObjectDisposedException>(() =>
            conversation.SendAsync("after dispose"));
    }

    [Fact]
    public async System.Threading.Tasks.Task CnetHarnessConversation_FailedSendPreservesCommittedHistory()
    {
        var fake = new FailSecondChatClient();
        using var conversation = new CnetHarnessConversation(
            fake, "system", maxRetainedTurns: 1,
            maxRetainedCharacters: 128);
        Assert.Equal("a1", await conversation.SendAsync("u1"));
        var before = conversation.Snapshot();

        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            conversation.SendAsync("u2"));

        Assert.Equal(before, conversation.Snapshot());
        Assert.Equal(1, conversation.RetainedTurnCount);
    }

    [Fact]
    public void Generate_ManyCalls_FreeEveryNativeGeneration()
    {
        var fake = new FakeNative { OpenReturn = 0, GenerationText = "ok" };
        using var session = CnetHarnessSession.Open(MakeConfig(), fake);
        var options = new CnetHarnessGenerateOptions
        {
            User = "bounded", Role = "memory", MaxTokens = 4,
        };

        for (int i = 0; i < 1000; ++i)
            _ = session.Generate(options);

        Assert.Equal(1000, fake.GenerationFreeCalls);
    }

    // ---- host environment parsing ----

    [Fact]
    public void CceHostConfig_Defaults_AreCpuAndMainGpuMinusOne()
    {
        var cfg = CceHostConfig.FromEnvironment(_ => null);
        Assert.Equal(CnetHarnessResource.Cpu, cfg.Resource);
        Assert.Equal(-1, cfg.MainGpu);
        Assert.True(cfg.ContextTokens >= 256u);
        Assert.True(cfg.BatchTokens <= cfg.ContextTokens);
    }

    [Fact]
    public void CceHostConfig_ParsesGpu1_FromEnv()
    {
        var env = new Dictionary<string, string?>
        {
            [CceHostConfig.EnvResourceMask] = "gpu1",
            [CceHostConfig.EnvMainGpu] = "0",
        };
        var cfg = CceHostConfig.FromEnvironment(k =>
            env.TryGetValue(k, out var v) ? v : null);
        Assert.Equal(CnetHarnessResource.Gpu1, cfg.Resource);
        Assert.Equal(0, cfg.MainGpu);
    }

    [Fact]
    public void CceHostConfig_RejectsMultiBitResource()
    {
        var env = new Dictionary<string, string?>
        {
            [CceHostConfig.EnvResourceMask] = "6",  /* 0b110 => Gpu0|Gpu1 */
        };
        Assert.Throws<ArgumentException>(() =>
            CceHostConfig.FromEnvironment(k =>
                env.TryGetValue(k, out var v) ? v : null));
    }

    [Fact]
    public void CceHostConfig_RejectsGarbageInteger()
    {
        var env = new Dictionary<string, string?>
        {
            [CceHostConfig.EnvThreads] = "not-a-number",
        };
        Assert.Throws<ArgumentException>(() =>
            CceHostConfig.FromEnvironment(k =>
                env.TryGetValue(k, out var v) ? v : null));
    }

    // ---- library resolver candidate paths ----

    [Fact]
    public void LibraryResolver_OverrideEnv_IsHandledOutsideFallbackCandidates()
    {
        var overrides = new Dictionary<string, string?>
        {
            [CnetHarnessLibraryResolver.EnvOverride] = "/some/absolute/libcnet_harness.so",
            [CnetHarnessLibraryResolver.EnvBinDir] = null,
        };
        string[] candidates = CnetHarnessLibraryResolver.EnumerateCandidates(k =>
            overrides.TryGetValue(k, out var v) ? v : null);
        Assert.DoesNotContain("/some/absolute/libcnet_harness.so", candidates);
    }

    [Fact]
    public void CceHostConfig_DoesNotMisreadDecimalTenAsHexGpu3()
    {
        var env = new Dictionary<string, string?>
        {
            [CceHostConfig.EnvResourceMask] = "10",
        };
        Assert.Throws<ArgumentException>(() =>
            CceHostConfig.FromEnvironment(k =>
                env.TryGetValue(k, out var v) ? v : null));
    }

    [Fact]
    public void CceHostConfig_ParsesExplicitHexResource()
    {
        var env = new Dictionary<string, string?>
        {
            [CceHostConfig.EnvResourceMask] = "0x10",
        };
        var cfg = CceHostConfig.FromEnvironment(k =>
            env.TryGetValue(k, out var v) ? v : null);
        Assert.Equal(CnetHarnessResource.Gpu3, cfg.Resource);
    }

    [Theory]
    [InlineData(CceHostConfig.EnvMainGpu, "-2")]
    [InlineData(CceHostConfig.EnvContextTokens, "255")]
    [InlineData(CceHostConfig.EnvThreads, "1025")]
    public void CceHostConfig_RejectsOutOfRangeValues(string key, string value)
    {
        var env = new Dictionary<string, string?> { [key] = value };
        Assert.Throws<ArgumentException>(() =>
            CceHostConfig.FromEnvironment(k =>
                env.TryGetValue(k, out var v) ? v : null));
    }

    [Fact]
    public void LibraryResolver_BinDirEnv_ContributesCandidate()
    {
        var overrides = new Dictionary<string, string?>
        {
            [CnetHarnessLibraryResolver.EnvOverride] = null,
            [CnetHarnessLibraryResolver.EnvBinDir] = "/some/bin",
        };
        string[] candidates = CnetHarnessLibraryResolver.EnumerateCandidates(k =>
            overrides.TryGetValue(k, out var v) ? v : null);
        Assert.Contains(candidates,
            c => c.Replace('\\', '/') == "/some/bin/libcnet_harness.so");
    }

    [Fact]
    public void LibraryResolver_NoEnv_IncludesAppBaseCandidates()
    {
        string[] candidates = CnetHarnessLibraryResolver.EnumerateCandidates(_ => null);
        Assert.NotEmpty(candidates);
        Assert.All(candidates, c => Assert.EndsWith("libcnet_harness.so", c));
    }

    // ---- fake chat/native invokers ----

    private sealed class RecordingChatClient : IChatClient, IDisposable
    {
        private readonly Queue<string> _replies;

        public RecordingChatClient(params string[] replies)
        {
            _replies = new Queue<string>(replies);
        }

        public List<(string Role, string Content)[]> Requests { get; } = new();
        public int DisposeCalls { get; private set; }

        public System.Threading.Tasks.Task<string> ChatAsync(
            (string Role, string Content)[] messages)
        {
            Requests.Add(((string Role, string Content)[])messages.Clone());
            return System.Threading.Tasks.Task.FromResult(_replies.Dequeue());
        }

        public void Dispose() => DisposeCalls++;
    }

    private sealed class FailSecondChatClient : IChatClient
    {
        private int _calls;

        public System.Threading.Tasks.Task<string> ChatAsync(
            (string Role, string Content)[] messages)
        {
            _ = messages;
            _calls++;
            return _calls == 1
                ? System.Threading.Tasks.Task.FromResult("a1")
                : System.Threading.Tasks.Task.FromException<string>(
                    new InvalidOperationException("injected failure"));
        }
    }

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
        public CnetHarnessSamplingMode LastSampling;
        public uint LastSeed;
        public ManualResetEventSlim? GenerateEntered;
        public ManualResetEventSlim? GenerateRelease;
        public int MaxConcurrentGenerations;
        private int _activeGenerations;

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
            int active = Interlocked.Increment(ref _activeGenerations);
            int observed;
            do
            {
                observed = Volatile.Read(ref MaxConcurrentGenerations);
            }
            while (active > observed &&
                   Interlocked.CompareExchange(ref MaxConcurrentGenerations,
                       active, observed) != observed);

            GenerateEntered?.Set();
            GenerateRelease?.Wait(TimeSpan.FromSeconds(2));
            try
            {
                LastRole = options.Role == IntPtr.Zero ? ""
                    : Marshal.PtrToStringUTF8(options.Role) ?? "";
                LastUser = options.User == IntPtr.Zero ? ""
                    : Marshal.PtrToStringUTF8(options.User) ?? "";
                LastSystem = options.System == IntPtr.Zero ? ""
                    : Marshal.PtrToStringUTF8(options.System) ?? "";
                LastSampling = options.Sampling;
                LastSeed = options.Seed;

                if (GenerateReturn != 0)
                {
                    generation = IntPtr.Zero;
                    return GenerateReturn;
                }
                /* Return a non-zero sentinel; ReadGeneration will look it up. */
                generation = new IntPtr(0x5678);
                return 0;
            }
            finally
            {
                Interlocked.Decrement(ref _activeGenerations);
            }
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

        public float GenerationEffectiveTemperature = 0.30f;
        public float GenerationEffectiveTopP = 0.85f;
        public uint  GenerationEffectiveTopK = 40u;
        public float GenerationEffectiveMinP = 0.05f;

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
                GenerationAicimoOverride,
                GenerationEffectiveTemperature,
                GenerationEffectiveTopP,
                GenerationEffectiveTopK,
                GenerationEffectiveMinP);
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
