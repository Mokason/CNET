using System.Net;
using System.Net.Http.Json;
using System.Text.Json;
using CNET.Llm.Engine.KvCache;
using CNET.Llm.Engine.PromptCache;
using CNET.Llm.Server;
using Xunit;

namespace CNET.Llm.Tests.ServerSecurity;

public sealed class ModelSwapTests
{
    [Fact]
    public async Task FailedPreparationRetainsUsableIncumbent()
    {
        var engine = new FixtureEngine();
        using var state = engine.CreateState();
        await Assert.ThrowsAsync<InvalidOperationException>(() => state.SwapModelAsync(
            _ => Task.FromException<ServerState>(new InvalidOperationException("fixture-load-failure")), CancellationToken.None));
        Assert.True(engine.DisposeCalls == 0 && state.IsReady, "MANAGED_SERVER_SWAP_RED failed_prepare_disposed_incumbent");
        await state.ExecuteAsync(() =>
        {
            state.Generator!.Generate("fixture", new() { MaxTokens = 1 });
            return Task.CompletedTask;
        }, CancellationToken.None);
        Assert.Equal(1, engine.Calls);
    }

    [Fact]
    public async Task CanceledCandidateIsDisposedAndIncumbentRetained()
    {
        var old = new FixtureEngine();
        var next = new FixtureEngine();
        using var state = old.CreateState();
        using var candidate = next.CreateState();
        using var cancellation = new CancellationTokenSource();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => state.SwapModelAsync(_ =>
        {
            cancellation.Cancel();
            return Task.FromResult(candidate);
        }, cancellation.Token));
        Assert.Equal(0, old.DisposeCalls);
        Assert.Equal(1, next.DisposeCalls);
        Assert.Same(old, state.Model);
        Assert.True(state.IsReady);
    }

    [Fact]
    public async Task SuccessfulAdoptionTransfersResourcesAndDetachesCandidate()
    {
        var old = new FixtureEngine();
        var next = new FixtureEngine();
        var draft = new FixtureEngine();
        using var state = old.CreateState();
        using var candidate = next.CreateState();
        candidate.Options = candidate.Options with { Model = "prepared-fixture" };
        candidate.LoadedModelPath = "private-fixture.gguf";
        candidate.PagedFactory = new PagedKvCacheFactory(1, 1, 1, maxTotalTokens: 32);
        candidate.PrefixCache = new PrefixCache(1);
        candidate.DraftModel = draft;
        candidate.DraftModelPath = "draft-fixture.gguf";
        var paged = candidate.PagedFactory;
        var prefix = candidate.PrefixCache;
        var generator = candidate.Generator;
        await state.SwapModelAsync(_ => Task.FromResult(candidate), CancellationToken.None);
        candidate.Dispose();
        Assert.Equal(1, old.DisposeCalls);
        Assert.Equal(0, next.DisposeCalls);
        Assert.Equal(0, draft.DisposeCalls);
        Assert.Same(paged, state.PagedFactory);
        Assert.Same(prefix, state.PrefixCache);
        Assert.Same(generator, state.Generator);
        Assert.Same(draft, state.DraftModel);
        Assert.Equal("prepared-fixture", state.Options.Model);
        Assert.Equal("private-fixture.gguf", state.LoadedModelPath);
        Assert.Equal("draft-fixture.gguf", state.DraftModelPath);
        Assert.Null(candidate.Model);
        Assert.True(state.IsReady);
        state.Dispose();
        Assert.Equal(1, next.DisposeCalls);
        Assert.Equal(1, draft.DisposeCalls);
    }

    [Fact]
    public async Task PreparationHoldsCanonicalGateForDirectCallers()
    {
        var old = new FixtureEngine();
        var next = new FixtureEngine();
        using var state = old.CreateState();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var swap = state.SwapModelAsync(async _ =>
        {
            entered.SetResult();
            await release.Task;
            return next.CreateState();
        }, CancellationToken.None);
        await entered.Task;
        bool ran = false;
        var request = state.ExecuteAsync(() => { ran = true; Assert.Same(next, state.Model); return Task.CompletedTask; }, CancellationToken.None);
        Assert.False(ran);
        Assert.Equal(0, old.DisposeCalls);
        release.SetResult();
        await Task.WhenAll(swap, request);
        Assert.True(ran);
    }

    [Fact]
    public async Task InvalidAndSelfCandidatesCannotDestroyIncumbent()
    {
        var old = new FixtureEngine();
        using var state = old.CreateState();
        await Assert.ThrowsAsync<InvalidOperationException>(() => state.SwapModelAsync(_ => Task.FromResult(state), CancellationToken.None));
        await Assert.ThrowsAsync<InvalidOperationException>(() => state.SwapModelAsync(
            _ => Task.FromResult(ServerStartup.CreateBareState(state.Options)), CancellationToken.None));
        Assert.Equal(0, old.DisposeCalls);
        Assert.True(state.IsReady);
    }

    [Fact]
    public async Task RetirementFailureKeepsCandidateActiveAndBlocksFurtherPreparation()
    {
        var old = new FixtureEngine { OnDispose = () => throw new InvalidOperationException("private-cleanup-fault") };
        var next = new FixtureEngine();
        await using var server = await ServerFixture.Start(state: old.CreateState());
        await server.State.SwapModelAsync(_ => Task.FromResult(next.CreateState()), CancellationToken.None);
        Assert.Same(next, server.State.Model);
        Assert.True(server.State.IsReady);
        Assert.True(server.State.RetirementCleanupFailed);
        bool prepared = false;
        await Assert.ThrowsAsync<InvalidOperationException>(() => server.State.SwapModelAsync(_ =>
        { prepared = true; return Task.FromResult(new FixtureEngine().CreateState()); }, CancellationToken.None));
        Assert.False(prepared);
        Assert.Equal(1, old.DisposeCalls);
        server.Authenticate(ServerFixture.AdminKey);
        using var props = await server.Client.GetAsync("/props");
        Assert.Equal(HttpStatusCode.OK, props.StatusCode);
        using var json = JsonDocument.Parse(await props.Content.ReadAsStringAsync());
        Assert.True(json.RootElement.GetProperty("retirement_cleanup_failed").GetBoolean());
        Assert.True(json.RootElement.GetProperty("is_ready").GetBoolean());
        using var refused = await server.Client.PostAsJsonAsync("/v1/models/load", new { model = "unused-fixture.gguf" });
        Assert.True(refused.StatusCode == HttpStatusCode.Conflict, "MANAGED_SERVER_SWAP_RED retirement_requires_restart_response");
    }

    [Fact]
    public async Task DisposeWaitsForActiveWorkAndRejectsNewCalls()
    {
        var engine = new FixtureEngine();
        var state = engine.CreateState();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var work = state.ExecuteAsync(async () =>
        {
            entered.SetResult();
            await release.Task;
            Assert.Equal(0, engine.DisposeCalls);
        }, CancellationToken.None);
        await entered.Task;
        var disposing = Task.Run(state.Dispose);
        release.SetResult();
        await Task.WhenAll(work, disposing);
        Assert.Equal(1, engine.DisposeCalls);
        await Assert.ThrowsAsync<ObjectDisposedException>(() => state.ExecuteAsync(() => Task.CompletedTask, CancellationToken.None));
        await Assert.ThrowsAsync<ObjectDisposedException>(() => state.SwapModelAsync(_ => Task.FromResult(new FixtureEngine().CreateState()), CancellationToken.None));
    }

    [Fact]
    public void FailedRealLoaderReleasesPrivateFixtureMappingOnLinux()
    {
        if (!OperatingSystem.IsLinux()) return; // /proc mapping evidence is Linux-specific.
        var directory = Directory.CreateTempSubdirectory("cnet-server-loader-");
        string path = Path.Combine(directory.FullName, "invalid-config.gguf");
        try
        {
            using (var file = File.Create(path))
            using (var writer = new BinaryWriter(file))
            {
                writer.Write(0x46554747u); writer.Write(3u); writer.Write(1ul); writer.Write(0ul);
                writer.Write(1ul); writer.Write((byte)'x'); writer.Write(1u); writer.Write(1ul);
                writer.Write(0u); writer.Write(0ul);
                while (file.Position % 32 != 0) writer.Write((byte)0);
                writer.Write(0f); // One mapped dummy tensor, intentionally no architecture metadata.
            }
            Assert.ThrowsAny<Exception>(() => ServerStartup.LoadModel(path, new ServerOptions { Model = path }));
            Assert.False(File.ReadLines("/proc/self/maps").Any(line => line.Contains(path, StringComparison.Ordinal)),
                "MANAGED_SERVER_SWAP_RED failed_load_mapping_leak");
        }
        finally { File.Delete(path); directory.Delete(); }
    }

    [Fact]
    public void DisposalIsIdempotent()
    {
        var engine = new FixtureEngine();
        var state = engine.CreateState();
        state.Dispose();
        state.Dispose();
        Assert.True(engine.DisposeCalls == 1, "MANAGED_SERVER_SWAP_RED double_dispose");
    }
}
