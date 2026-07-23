using CNET.Cce.CnetHarness;
using CNET.Cce.Llm.Memory;
using CNET.Cce.Llm.Routing;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// Rung 4 routing: certified knowledge serves before the model, under
/// model-free guards — subject grounding, ledger-certification, reliability —
/// and the correction feedback loop demotes units that earn corrections.
/// </summary>
public sealed class RecordRouterTests : IDisposable
{
    private readonly string _dir;

    public RecordRouterTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cnet-router", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(RecordsDir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private string RecordsDir => Path.Combine(_dir, "records");
    private string LedgerPath => Path.Combine(_dir, "gaps.txt");

    /// <summary>A record + a ledger line that certifies it CLOSED with its unit.</summary>
    private void Seal(string tag, string recordText)
    {
        File.WriteAllText(Path.Combine(RecordsDir, tag + ".txt"), recordText);
        File.AppendAllText(LedgerPath,
            $"0 2 1 1 1 256 1 w_cur 1 256 1 {tag} - rec_{tag} - acq_{tag} 1 0\n");
    }

    private RecordRouter NewRouter() => new(RecordsDir, LedgerPath);

    // ─────────────── guards ───────────────

    [Fact]
    public void Routes_WhenGrounded_AndCertified()
    {
        Seal("skill_corr_t1", "#v2\nthe beacon frequency at the lighthouse is megahertz\n");

        var serve = NewRouter().TryRoute("what was the beacon frequency at the lighthouse?");

        Assert.NotNull(serve);
        Assert.Equal("acq_skill_corr_t1", serve!.UnitName);
        Assert.Contains("beacon", serve.GroundedTerms);
        Assert.DoesNotContain("#v2", serve.RecordText);          // marker stripped
        Assert.Contains("megahertz", serve.RecordText);          // verbatim content
    }

    [Fact]
    public void Declines_WhenNotGrounded()
    {
        Seal("skill_corr_t1", "#v2\nthe beacon frequency at the lighthouse is megahertz\n");
        // one shared term is not subject grounding
        Assert.Null(NewRouter().TryRoute("tell me about a beacon of hope"));
    }

    [Fact]
    public void Declines_WithoutLedgerCertification()
    {
        // record exists, but nothing sealed it — no certificate, no serving
        File.WriteAllText(Path.Combine(RecordsDir, "skill_corr_t1.txt"),
            "#v2\nthe beacon frequency at the lighthouse is megahertz\n");
        File.WriteAllText(LedgerPath,
            "0 1 1 0 1 256 1 w_cur 1 256 1 skill_corr_t1 - - waiting_oracle - 0 0\n");

        Assert.Null(NewRouter().TryRoute("what was the beacon frequency at the lighthouse?"));
    }

    [Fact]
    public void BestGrounding_Wins_WhenSeveralQualify()
    {
        Seal("skill_corr_aa", "#v2\nthe beacon frequency setting\n");
        Seal("skill_vrf_bb", "#v2\nthe beacon frequency at the lighthouse tower setting\n");

        var serve = NewRouter().TryRoute(
            "what is the beacon frequency setting at the lighthouse tower?");
        Assert.Equal("acq_skill_vrf_bb", serve!.UnitName);
    }

    // ─────────────── the learned part: outcome feedback ───────────────

    [Fact]
    public void CorrectionAfterServe_DemotesTheUnit_Durably()
    {
        Seal("skill_corr_t1", "#v2\nthe beacon frequency at the lighthouse is megahertz\n");
        var router = NewRouter();

        // Serve once, get corrected: 1 served / 1 corrected -> reliability 0.
        Assert.NotNull(router.TryRoute("what was the beacon frequency at the lighthouse?"));
        router.ObserveUserTurn("wrong, that is not the frequency");

        Assert.Null(router.TryRoute("what was the beacon frequency at the lighthouse?"));

        // Durable across router instances (routing.json sidecar).
        Assert.Null(NewRouter().TryRoute("what was the beacon frequency at the lighthouse?"));
        var stats = NewRouter().Stats["acq_skill_corr_t1"];
        Assert.Equal((1, 1), stats);
    }

    [Fact]
    public void NonCorrectionTurn_IsNotFeedback_AndWindowIsOneTurn()
    {
        Seal("skill_corr_t1", "#v2\nthe beacon frequency at the lighthouse is megahertz\n");
        var router = NewRouter();

        Assert.NotNull(router.TryRoute("what was the beacon frequency at the lighthouse?"));
        router.ObserveUserTurn("thanks, that is helpful");     // praise, not correction
        router.ObserveUserTurn("wrong about something else");  // window already closed

        Assert.Equal((1, 0), router.Stats["acq_skill_corr_t1"]);
        Assert.NotNull(router.TryRoute("what was the beacon frequency at the lighthouse?"));
    }

    // ─────────────── the full chain in MemorySession ───────────────

    private sealed class MustNotRun : ICnetInferenceSession
    {
        public CnetHarnessGenerationResult Generate(CnetHarnessGenerateOptions options)
            => throw new InvalidOperationException("model must not be consulted");
        public CnetHarnessRouteInfo ProbeRoute(string role,
            CnetHarnessSamplingMode overrideMode = CnetHarnessSamplingMode.Auto)
            => throw new NotSupportedException();
        public void Dispose() { }
    }

    [Fact]
    public void MemorySession_ServesCertified_BeforeModel_AndRemembers()
    {
        Seal("skill_corr_t1", "#v2\nthe beacon frequency at the lighthouse is megahertz\n");
        string storePath = Path.Combine(_dir, "s.jsonl");
        using var store = BlobStore.Open(storePath);
        var memory = new ConversationMemory(store, s => s.Length / 4 + 1);
        var ghost = new MemorySession(new MustNotRun(), memory, 4096, s => s.Length / 4 + 1)
        {
            Router = NewRouter(),
        };

        var r = ghost.Generate(null, "what was the beacon frequency at the lighthouse?");

        Assert.Equal("acq_skill_corr_t1", r.CertifiedUnit);
        Assert.Contains("megahertz", r.Result.Text);
        Assert.False(r.Exact);
        Assert.Equal(2, store.Count);                    // exchange remembered
    }

    [Fact]
    public void ExactLane_StillOutranksCertified()
    {
        Seal("skill_corr_t1", "#v2\nwhat is two plus two arithmetic lesson\n");
        string storePath = Path.Combine(_dir, "s2.jsonl");
        using var store = BlobStore.Open(storePath);
        var memory = new ConversationMemory(store, s => s.Length / 4 + 1);
        var ghost = new MemorySession(new MustNotRun(), memory, 4096, s => s.Length / 4 + 1)
        {
            Router = NewRouter(),
        };

        var r = ghost.Generate(null, "what is 2+2");
        Assert.True(r.Exact);                            // computed beats certified
        Assert.Null(r.CertifiedUnit);
    }
}
