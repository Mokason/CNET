using CNET.Cce.CnetHarness;
using CNET.Cce.Llm.Memory;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// Operational chain of thought: documents ingest as recallable material, and
/// the action protocol (RECALL / READ / CALC) lets the model act — grounded
/// steps with receipts — before answering, under one shared round budget.
/// </summary>
public sealed class ActionProtocolTests : IDisposable
{
    private readonly string _dir;

    public ActionProtocolTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cnet-actions", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private string StorePath() => Path.Combine(_dir, "s.jsonl");

    private sealed class ScriptedSession(params string[] outputs) : ICnetInferenceSession
    {
        private readonly Queue<string> _script = new(outputs);
        public List<CnetHarnessGenerateOptions> Calls { get; } = new();

        public CnetHarnessGenerationResult Generate(CnetHarnessGenerateOptions options)
        {
            Calls.Add(options);
            string text = _script.Count > 0 ? _script.Dequeue() : "(exhausted)";
            return new CnetHarnessGenerationResult(text, 10, 5, 1, 1, 0, 0,
                CnetHarnessSamplingMode.Deterministic, false, 0, 1, 0, 0);
        }

        public CnetHarnessRouteInfo ProbeRoute(string role,
            CnetHarnessSamplingMode overrideMode = CnetHarnessSamplingMode.Auto) =>
            throw new NotSupportedException();

        public void Dispose() { }
    }

    private (MemorySession Ghost, ConversationMemory Memory) NewGhost(
        BlobStore store, ScriptedSession session)
    {
        var memory = new ConversationMemory(store, t => t.Length / 4 + 1);
        var ghost = new MemorySession(session, memory, 8192, t => t.Length / 4 + 1);
        return (ghost, memory);
    }

    // ─────────────── document ingestion ───────────────

    [Fact]
    public void IngestDocument_Chunks_WithSourceHeaders_AndDocRole()
    {
        using var store = BlobStore.Open(StorePath());
        var memory = new ConversationMemory(store, t => t.Length / 4 + 1);

        string doc = string.Join("\n\n", Enumerable.Range(0, 12).Select(i =>
            $"Chapter {i}: the lighthouse manual describes procedure number {i} " +
            "in exhaustive detail across many pages of operational guidance."));
        var ids = memory.IngestDocument("manual.txt", doc);

        Assert.True(ids.Count > 1, "long document should chunk");
        MemoryBlob first = store.Get(ids[0])!;
        Assert.Equal("doc", first.Role);
        Assert.StartsWith("[doc:manual.txt §1/", first.Text);
        Assert.Equal("doc:manual.txt", first.SessionId);
    }

    [Fact]
    public void Documents_NeverFloodRecentTurns()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession("a fine answer");
        var (ghost, memory) = NewGhost(store, session);

        memory.IngestDocument("manual.txt",
            "the lighthouse beacon frequency is exactly nine megahertz");
        var r = ghost.Generate(null, "hello there, how are you?");

        // Unrelated greeting: the doc must not ride along as a recent turn.
        Assert.DoesNotContain("§1/", session.Calls[0].System ?? "");
        Assert.NotNull(r.Result.Text);
    }

    [Fact]
    public void Documents_AreRecallable_ByKeyword()
    {
        using var store = BlobStore.Open(StorePath());
        var memory = new ConversationMemory(store, t => t.Length / 4 + 1);
        memory.IngestDocument("manual.txt",
            "the lighthouse beacon frequency is exactly nine megahertz");

        var hits = store.Recall("lighthouse beacon frequency", 3);
        Assert.NotEmpty(hits);
        Assert.Equal("doc", hits[0].Role);
    }

    // ─────────────── the actions ───────────────

    [Fact]
    public void ReadAction_ServesDocumentSections_WithReceipts()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession(
            "READ: lighthouse beacon manual",
            "Nine megahertz, per the manual.");
        var (ghost, memory) = NewGhost(store, session);
        long docId = memory.IngestDocument("manual.txt",
            "the lighthouse beacon frequency is exactly nine megahertz")[0];

        // Question shares no keywords with the document — the gate misses,
        // so the READ action is what finds it (the scenario READ exists for).
        var r = ghost.Generate(null, "what does the tower emit for ships at night?");

        var round = Assert.Single(r.Lookups);
        Assert.Equal("read", round.Kind);
        Assert.Contains(docId, round.BlobIds);
        Assert.Contains(docId, r.UsedBlobIds);
        Assert.Contains("### Document sections", session.Calls[1].System);
        Assert.Contains("nine megahertz", session.Calls[1].System);
        Assert.Equal("Nine megahertz, per the manual.", r.Result.Text);
    }

    [Fact]
    public void ReadAction_IgnoresConversationBlobs()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession(
            "READ: lighthouse beacon frequency",
            "The documents do not cover it.");
        var (ghost, memory) = NewGhost(store, session);
        // Conversation mentions it; no DOCUMENT does.
        store.Append("old", 0, "user", "the lighthouse beacon frequency is nine", 8);

        var r = ghost.Generate(null, "what does the manual say about the beacon?");

        var round = Assert.Single(r.Lookups);
        Assert.Empty(round.BlobIds);
        Assert.Contains("no document section matches", session.Calls[1].System);
    }

    [Fact]
    public void CalcAction_ComputesExactly_AndDeclinesHonestly()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession(
            "CALC: 47 * 89 + 17",
            "CALC: the meaning of life",
            "4200, computed exactly.");
        var (ghost, _) = NewGhost(store, session);

        var r = ghost.Generate(null, "what is 47*89+17, and life?");

        Assert.Equal(2, r.Lookups.Count);
        Assert.Equal("calc", r.Lookups[0].Kind);
        Assert.Equal("4200", r.Lookups[0].Output);
        Assert.Contains("47 * 89 + 17 = 4200", session.Calls[1].System);
        Assert.Null(r.Lookups[1].Output);
        Assert.Contains("declined (not pure arithmetic", session.Calls[2].System);
    }

    [Fact]
    public void MixedActions_ShareOneRoundBudget()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession(
            "RECALL: alpha", "READ: beta", "CALC: 1+1",   // budget is 2
            "final answer");
        var memory = new ConversationMemory(store, t => t.Length / 4 + 1);
        var ghost = new MemorySession(session, memory, 8192, t => t.Length / 4 + 1,
                                      maxLookupRounds: 2);

        var r = ghost.Generate(null, "question");

        Assert.Equal(2, r.Lookups.Count);                       // recall + read
        // budget exhausted mid-CALC: forced answer, scaffolding never shown
        Assert.Contains("No more actions", session.Calls[3].System);
        Assert.Equal("final answer", r.Result.Text);
    }

    // ─────────────── parser ───────────────

    [Theory]
    [InlineData("READ: lighthouse manual", "read", "lighthouse manual")]
    [InlineData("CALC: 2+2", "calc", "2+2")]
    [InlineData("RECALL: wifi password", "recall", "wifi password")]
    [InlineData("  \n READ: \"quoted doc\" ", "read", "quoted doc")]
    public void TryParseAction_RecognizesAllKinds(string text, string kind, string arg)
    {
        Assert.True(MemorySession.TryParseAction(text, out string k, out string a));
        Assert.Equal(kind, k);
        Assert.Equal(arg, a);
    }

    [Theory]
    [InlineData("FETCH: something")]         // unknown verb: it is the answer
    [InlineData("I would READ: this")]       // not at line start
    [InlineData("READ:")]                    // empty arg
    [InlineData("read: lowercase")]          // protocol is exact
    public void TryParseAction_RejectsNonActions(string text) =>
        Assert.False(MemorySession.TryParseAction(text, out _, out _));
}
