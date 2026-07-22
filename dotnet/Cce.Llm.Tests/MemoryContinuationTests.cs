using CNET.Cce.CnetHarness;
using CNET.Cce.Llm.Memory;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// Continuation of budget-truncated answers: a capped reply arms the next
/// turn, and a "continue" resumes at the exact cut instead of restarting from
/// the top (the observed live failure with a 512-token quest JSON).
/// </summary>
public sealed class MemoryContinuationTests : IDisposable
{
    private readonly string _dir;

    public MemoryContinuationTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cnet-continue", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private string StorePath() => Path.Combine(_dir, "c.jsonl");

    /// <summary>Inner session that reports every answer as exactly maxTokens long
    /// (budget-capped) or clearly under it, and records prompts.</summary>
    private sealed class ScriptedSession : ICnetInferenceSession
    {
        private readonly Queue<(string Text, bool Capped)> _script;
        public List<CnetHarnessGenerateOptions> Calls { get; } = new();

        public ScriptedSession(params (string, bool)[] outputs) => _script = new(outputs);

        public CnetHarnessGenerationResult Generate(CnetHarnessGenerateOptions options)
        {
            Calls.Add(options);
            var (text, capped) = _script.Count > 0 ? _script.Dequeue() : ("(exhausted)", false);
            uint generated = capped ? options.MaxTokens : Math.Max(1, options.MaxTokens / 4);
            return new CnetHarnessGenerationResult(text, 10, generated, 1, 1, 0, 0,
                CnetHarnessSamplingMode.Deterministic, false, 0, 1, 0, 0);
        }

        public CnetHarnessRouteInfo ProbeRoute(string role,
            CnetHarnessSamplingMode overrideMode = CnetHarnessSamplingMode.Auto) =>
            throw new NotSupportedException();

        public void Dispose() { }
    }

    private MemorySession NewGhost(BlobStore store, ScriptedSession session) =>
        new(session, new ConversationMemory(store, s => s.Length / 4 + 1), 4096,
            s => s.Length / 4 + 1);

    [Fact]
    public void CappedAnswer_ReportsTruncated_AndContinueResumesAtCut()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession(
            ("{ \"quest\": { \"stages\": [ { \"stage\": 0, \"logEntry\": \"cut here", true),
            ("\" } ] } }", false));
        var ghost = NewGhost(store, session);

        var first = ghost.Generate(null, "write me a quest json");
        Assert.True(first.Truncated);

        var second = ghost.Generate(null, "continue");
        Assert.False(second.Truncated);

        string resumePrompt = session.Calls[1].System!;
        Assert.Contains("### Continuation", resumePrompt);
        Assert.Contains("cut here", resumePrompt);                    // the exact tail
        Assert.Contains("Do not repeat anything", resumePrompt);
        Assert.DoesNotContain("### Memory lookup", resumePrompt);     // loop off while resuming
    }

    [Fact]
    public void CompletedAnswer_DoesNotArmContinuation()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession(
            ("a complete short answer", false),
            ("a normal turn", false));
        var ghost = NewGhost(store, session);

        var first = ghost.Generate(null, "quick question");
        Assert.False(first.Truncated);

        ghost.Generate(null, "continue");
        Assert.DoesNotContain("### Continuation", session.Calls[1].System ?? "");
    }

    /// <summary>"Continue" refers only to the immediately previous turn.</summary>
    [Fact]
    public void InterveningCompletedTurn_ClearsPendingContinuation()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession(
            ("truncated tex", true),
            ("short complete reply", false),
            ("normal turn", false));
        var ghost = NewGhost(store, session);

        ghost.Generate(null, "long thing please");
        ghost.Generate(null, "unrelated quick question");
        ghost.Generate(null, "continue");

        Assert.DoesNotContain("### Continuation", session.Calls[2].System ?? "");
    }

    /// <summary>A resume that truncates again re-arms with the NEW tail.</summary>
    [Fact]
    public void ChainedContinues_AnchorOnTheLatestChunk()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession(
            ("chunk-one ends at ALPHA", true),
            ("chunk-two ends at BRAVO", true),
            ("chunk-three done.", false));
        var ghost = NewGhost(store, session);

        var r1 = ghost.Generate(null, "write something long");
        var r2 = ghost.Generate(null, "continue");
        var r3 = ghost.Generate(null, "continue");

        Assert.True(r1.Truncated);
        Assert.True(r2.Truncated);
        Assert.False(r3.Truncated);
        static string ContinuationBlock(string system)
        {
            int start = system.IndexOf("### Continuation", StringComparison.Ordinal);
            Assert.True(start >= 0, "no continuation block");
            int end = system.IndexOf("Resume EXACTLY", start, StringComparison.Ordinal);
            return system[start..end];
        }
        Assert.Contains("ALPHA", ContinuationBlock(session.Calls[1].System!));
        Assert.Contains("BRAVO", ContinuationBlock(session.Calls[2].System!));
        // The anchor is the NEW tail only — the old one lives in recent turns,
        // not in the continuation block.
        Assert.DoesNotContain("ALPHA", ContinuationBlock(session.Calls[2].System!));
    }

    [Fact]
    public void LongTail_IsClippedToTheEnd()
    {
        using var store = BlobStore.Open(StorePath());
        string longText = new string('x', 2000) + " THE-ACTUAL-CUT";
        var session = new ScriptedSession((longText, true), ("done", false));
        var ghost = NewGhost(store, session);

        ghost.Generate(null, "go");
        ghost.Generate(null, "continue");

        string resumePrompt = session.Calls[1].System!;
        int blockStart = resumePrompt.IndexOf("### Continuation", StringComparison.Ordinal);
        string block = resumePrompt[blockStart..];
        Assert.Contains("THE-ACTUAL-CUT", block);
        Assert.DoesNotContain(new string('x', 700), block);   // head of the reply clipped
    }

    [Theory]
    [InlineData("continue", true)]
    [InlineData("Continue!", true)]
    [InlineData("continue the json from stage 4", true)]
    [InlineData("go on", true)]
    [InlineData("keep going...", true)]
    [InlineData("resume", true)]
    [InlineData("finish", true)]
    [InlineData("carry on", true)]
    [InlineData("can you continue", false)]
    [InlineData("should i continue with the plan?", false)]
    [InlineData("tell me about continuous integration", false)]
    [InlineData("what next?", false)]
    public void IsContinueRequest_IsDeliberatelyNarrow(string user, bool expected) =>
        Assert.Equal(expected, MemorySession.IsContinueRequest(user));
}
