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

    /// <summary>Manual-path ghost: auto-continue off, as before it existed.</summary>
    private MemorySession NewGhost(BlobStore store, ScriptedSession session) =>
        new(session, new ConversationMemory(store, s => s.Length / 4 + 1), 4096,
            s => s.Length / 4 + 1, maxAutoContinues: 0);

    private MemorySession NewAutoGhost(BlobStore store, ScriptedSession session, int rounds) =>
        new(session, new ConversationMemory(store, s => s.Length / 4 + 1), 4096,
            s => s.Length / 4 + 1, maxAutoContinues: rounds);

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

    // ─────────────── auto-continue ───────────────

    /// <summary>The headline behavior: capped chunks are resumed and stitched
    /// into one answer without the user typing anything.</summary>
    [Fact]
    public void AutoContinue_StitchesChunksIntoOneAnswer()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession(
            ("{ \"stages\": [ { \"stage\": 0", true),
            (" }, { \"stage\": 1", true),
            (" } ] }", false));
        var ghost = NewAutoGhost(store, session, rounds: 3);

        var r = ghost.Generate(null, "write the quest json");

        Assert.Equal(3, session.Calls.Count);
        Assert.Equal("{ \"stages\": [ { \"stage\": 0 }, { \"stage\": 1 } ] }", r.Result.Text);
        Assert.False(r.Truncated);
        Assert.Equal(2, r.AutoContinues);

        // Round 2's resume block quotes the WHOLE answer so far — the
        // in-progress answer is not in recent turns, unlike manual continue.
        string secondResume = session.Calls[2].System!;
        Assert.Contains("### Continuation", secondResume);
        Assert.Contains("stage\": 0", secondResume);
        Assert.Contains("stage\": 1", secondResume);
        Assert.DoesNotContain("### Memory lookup", secondResume);
    }

    [Fact]
    public void AutoContinue_StoresTheStitchedWhole_AsOneMemoryTurn()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession(
            ("alpha-part", true), ("-omega-end", false));
        var ghost = NewAutoGhost(store, session, rounds: 3);

        ghost.Generate(null, "go");

        // user blob + assistant blob(s) holding the full text, no fragments.
        var all = new List<string>();
        for (long i = 1; i <= store.Count; i++)
            if (store.Get(i) is { } b) all.Add($"{b.Role}:{b.Text}");
        Assert.Contains("assistant:alpha-part-omega-end", all);
        Assert.DoesNotContain(all, t => t == "assistant:alpha-part");
    }

    /// <summary>Cap exhausted and still unfinished: report truncated and arm
    /// the manual path with the stitched text.</summary>
    [Fact]
    public void AutoContinue_CapExhausted_ArmsManualContinue()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession(
            ("one", true), ("-two", true), ("-three", true),
            ("-four is the end", false));
        var ghost = NewAutoGhost(store, session, rounds: 2);

        var r = ghost.Generate(null, "go");
        Assert.True(r.Truncated);
        Assert.Equal(2, r.AutoContinues);
        Assert.Equal("one-two-three", r.Result.Text);

        var r2 = ghost.Generate(null, "continue");
        Assert.Equal("-four is the end", r2.Result.Text);
        Assert.Contains("one-two-three", session.Calls[3].System!);   // manual anchor = stitched tail
        Assert.False(r2.Truncated);
    }

    [Fact]
    public void AutoContinue_AggregatesTokenAccounting()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession(("a", true), ("b", true), ("c", false));
        var ghost = NewAutoGhost(store, session, rounds: 3);

        var r = ghost.Generate(null, "go", maxTokens: 256);

        // Two capped rounds at 256 + one final at 64 (ScriptedSession: max/4).
        Assert.Equal(256u + 256u + 64u, r.Result.GeneratedTokens);
    }

    /// <summary>
    /// A round that comes back empty (a thinking model that ignored
    /// think:false and deliberated through its whole budget — observed live)
    /// is retried once with a no-deliberation nudge and double budget.
    /// </summary>
    [Fact]
    public void AutoContinue_EmptyRound_RetriesWithNudgeAndDoubleBudget()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession(
            ("real text", true), ("   ", true), (" salvaged end", false));
        var ghost = NewAutoGhost(store, session, rounds: 5);

        var r = ghost.Generate(null, "go", maxTokens: 300);

        Assert.Equal(3, session.Calls.Count);
        Assert.Equal("real text salvaged end", r.Result.Text);
        Assert.False(r.Truncated);
        Assert.Equal(300u, session.Calls[1].MaxTokens);
        Assert.Equal(600u, session.Calls[2].MaxTokens);           // doubled for the retry
        Assert.Contains("Do not spend tokens deliberating", session.Calls[2].System!);
        Assert.False(session.Calls[2].Think);
    }

    [Fact]
    public void AutoContinue_TwoEmptyRounds_StopTheLoop()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession(
            ("real text", true), ("   ", true), ("", true), ("never reached", false));
        var ghost = NewAutoGhost(store, session, rounds: 5);

        var r = ghost.Generate(null, "go");

        Assert.Equal(3, session.Calls.Count);      // round + retry, then stop paying
        Assert.Equal("real text", r.Result.Text);  // whitespace never stitched
        Assert.True(r.Truncated);                  // manual path armed
    }

    /// <summary>Auto-continue composes with the lookup loop: search first,
    /// then the (post-lookup) answer is resumed when capped.</summary>
    [Fact]
    public void AutoContinue_ComposesWithModelLookups()
    {
        using var store = BlobStore.Open(StorePath());
        store.Append("old", 0, "user", "the quest hero is named Torvald", 8);
        var session = new ScriptedSession(
            ("RECALL: quest hero name", false),
            ("Torvald's saga begins", true),
            (" and ends in glory.", false));
        var ghost = new MemorySession(session,
            new ConversationMemory(store, s => s.Length / 4 + 1), 4096,
            s => s.Length / 4 + 1, maxLookupRounds: 2, maxAutoContinues: 3);

        var r = ghost.Generate(null, "write the hero saga");

        Assert.Single(r.Lookups);
        Assert.Equal(1, r.AutoContinues);
        Assert.Equal("Torvald's saga begins and ends in glory.", r.Result.Text);
        Assert.False(r.Truncated);
    }

    // ─────────────── hidden-reasoning suppression on resumes ───────────────

    /// <summary>
    /// The live failure that motivated Think plumbing: a thinking model spent
    /// its entire 512-token budget deliberating about the resume and emitted
    /// nothing. Resume rounds — manual and auto — must request think=false;
    /// ordinary turns must leave the backend default untouched.
    /// </summary>
    [Fact]
    public void ResumeRounds_SuppressHiddenReasoning_OrdinaryTurnsDoNot()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new ScriptedSession(
            ("chunk one", true),      // ordinary turn, capped -> auto resume
            (" chunk two", true),     // auto resume, still capped (cap: 1)
            (" tail", false));        // manual "continue" turn
        var ghost = NewAutoGhost(store, session, rounds: 1);

        ghost.Generate(null, "write something long");
        ghost.Generate(null, "continue");

        Assert.Null(session.Calls[0].Think);          // ordinary turn: backend default
        Assert.False(session.Calls[1].Think);         // auto resume
        Assert.False(session.Calls[2].Think);         // manual resume
    }

    // ─────────────── structural continuation ───────────────

    /// <summary>Scripted session that supports structural resumes.</summary>
    private sealed class StructuralSession : ICnetInferenceSession
    {
        private readonly Queue<(string Text, bool Capped)> _script;
        public List<CnetHarnessGenerateOptions> Calls { get; } = new();
        public bool SupportsContinuation => true;

        public StructuralSession(params (string, bool)[] outputs) => _script = new(outputs);

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

    /// <summary>
    /// On a continuation-capable backend the partial answer rides as a real
    /// assistant turn (ContinueFrom), not as a quote inside the system prompt —
    /// quote-based resumes degraded live as the partial grew, until the model
    /// restarted with fresh content mid-string.
    /// </summary>
    [Fact]
    public void StructuralAutoResume_SendsPartialAsAssistantTurn()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new StructuralSession(
            ("part-one", true), (" part-two", true), (" done.", false));
        var ghost = new MemorySession(session,
            new ConversationMemory(store, s => s.Length / 4 + 1), 4096,
            s => s.Length / 4 + 1, maxAutoContinues: 5);

        var r = ghost.Generate(null, "write something long");

        Assert.Equal("part-one part-two done.", r.Result.Text);
        Assert.False(r.Truncated);
        Assert.Equal(2, r.AutoContinues);

        Assert.Null(session.Calls[0].ContinueFrom);                 // ordinary turn
        Assert.Equal("part-one", session.Calls[1].ContinueFrom);
        Assert.Equal("part-one part-two", session.Calls[2].ContinueFrom);
        Assert.StartsWith("Continue your message exactly", session.Calls[1].User);
        Assert.DoesNotContain("### Continuation", session.Calls[1].System ?? "");
        Assert.False(session.Calls[1].Think);
    }

    [Fact]
    public void StructuralManualContinue_AlsoUsesAssistantTurn()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new StructuralSession(
            ("cut-off-here", true), (" and the rest.", false));
        var ghost = new MemorySession(session,
            new ConversationMemory(store, s => s.Length / 4 + 1), 4096,
            s => s.Length / 4 + 1, maxAutoContinues: 0);   // manual path only

        var r1 = ghost.Generate(null, "go");
        Assert.True(r1.Truncated);

        var r2 = ghost.Generate(null, "continue");
        Assert.Equal(" and the rest.", r2.Result.Text);
        Assert.Equal("cut-off-here", session.Calls[1].ContinueFrom);
        Assert.Contains("continue", session.Calls[1].User);          // user's own words kept
        Assert.Contains("Continue your message exactly", session.Calls[1].User);
        Assert.DoesNotContain("### Continuation", session.Calls[1].System ?? "");
    }

    [Fact]
    public void StructuralEmptyRound_RetriesWithNudge()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new StructuralSession(
            ("part-one", true), ("  ", true), (" salvaged.", false));
        var ghost = new MemorySession(session,
            new ConversationMemory(store, s => s.Length / 4 + 1), 4096,
            s => s.Length / 4 + 1, maxAutoContinues: 5);

        var r = ghost.Generate(null, "go", maxTokens: 300);

        Assert.Equal("part-one salvaged.", r.Result.Text);
        Assert.Equal(600u, session.Calls[2].MaxTokens);
        Assert.Contains("deliberating", session.Calls[2].User);
        Assert.Equal("part-one", session.Calls[2].ContinueFrom);     // same anchor
    }

    // ─────────────── seam overlap trimming ───────────────

    [Theory]
    [InlineData("\"isStart", "\"isStart\": false", "\"isStart\": false")]   // live case: 8-char dup
    [InlineData("ends here", " and continues", "ends here and continues")]      // clean seam untouched
    [InlineData("say the", "the answer", "say thethe answer")]                  // <6 chars: never trimmed
    public void SeamOverlap_IsTrimmedOnlyWhenUnambiguous(string tail, string next, string expected)
    {
        string stitched = tail + MemorySession.TrimSeamOverlap(tail, next);
        Assert.Equal(expected == "\"isStart\": false" ? tail + "\": false" : expected, stitched);
    }

    [Fact]
    public void AutoContinue_TrimsDuplicatedSeam()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new StructuralSession(
            ("\"quest\": { \"isStart", true),
            ("\"isStart\": false }", false));
        var ghost = new MemorySession(session,
            new ConversationMemory(store, s => s.Length / 4 + 1), 4096,
            s => s.Length / 4 + 1, maxAutoContinues: 3);

        var r = ghost.Generate(null, "go");
        Assert.Equal("\"quest\": { \"isStart\": false }", r.Result.Text);
    }

    /// <summary>The live seam: cut at "type inside an open ```json block; the
    /// resume re-emitted fence markers before continuing with perfect content.</summary>
    [Theory]
    [InlineData("```json\n{ \"type",      "```json\n\": \"Interact\"",       "\": \"Interact\"")]
    [InlineData("```json\n{ \"type",      "\n```\n```json\n\": \"x\"",     "\": \"x\"")]
    [InlineData("no open fence here",       "```json\ncontent",                   "```json\ncontent")]
    [InlineData("```json\nabc\n```\nok", "```python\nnew block",               "```python\nnew block")]
    public void FenceChurn_IsStrippedOnlyInsideAnOpenFence(string soFar, string next, string expected) =>
        Assert.Equal(expected, MemorySession.CleanResumeChunk(soFar, next));

    /// <summary>
    /// A natural stop inside an open code fence is not finished (observed
    /// live: the model emitted EOS with 11 unclosed braces). The unclosed
    /// fence triggers a resume round even though the token budget was not hit.
    /// </summary>
    [Fact]
    public void NaturalStop_InsideOpenFence_StillResumes()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new StructuralSession(
            ("```json\n{ \"a\": 1", false),      // natural stop, fence open
            (" }\n```", false));                    // closes it
        var ghost = new MemorySession(session,
            new ConversationMemory(store, s => s.Length / 4 + 1), 4096,
            s => s.Length / 4 + 1, maxAutoContinues: 3);

        var r = ghost.Generate(null, "json please");

        Assert.Equal(1, r.AutoContinues);
        Assert.Equal("```json\n{ \"a\": 1 }\n```", r.Result.Text);
        Assert.False(r.Truncated);
    }

    [Fact]
    public void NaturalStop_WithBalancedFences_DoesNotResume()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new StructuralSession(("```json\n{}\n``` done", false));
        var ghost = new MemorySession(session,
            new ConversationMemory(store, s => s.Length / 4 + 1), 4096,
            s => s.Length / 4 + 1, maxAutoContinues: 3);

        var r = ghost.Generate(null, "json please");
        Assert.Equal(0, r.AutoContinues);
        Assert.False(r.Truncated);
    }

    [Fact]
    public void ResumeRounds_UseFocusedSampling_UnlessDeterministic()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new StructuralSession(("cut", true), (" done", false));
        var ghost = new MemorySession(session,
            new ConversationMemory(store, s => s.Length / 4 + 1), 4096,
            s => s.Length / 4 + 1, maxAutoContinues: 3);

        ghost.Generate(null, "go", sampling: CnetHarnessSamplingMode.Balanced);

        Assert.Equal(CnetHarnessSamplingMode.Balanced, session.Calls[0].Sampling);
        Assert.Equal(CnetHarnessSamplingMode.Focused, session.Calls[1].Sampling);

        var session2 = new StructuralSession(("cut", true), (" done", false));
        var ghost2 = new MemorySession(session2,
            new ConversationMemory(store, s => s.Length / 4 + 1), 4096,
            s => s.Length / 4 + 1, maxAutoContinues: 3);
        ghost2.Generate(null, "go", sampling: CnetHarnessSamplingMode.Deterministic);
        Assert.Equal(CnetHarnessSamplingMode.Deterministic, session2.Calls[1].Sampling);
    }

    /// <summary>The exact live seam of run A: cut inside '"id": "', resume
    /// re-emitted a fence marker AND re-typed the fragment behind fresh
    /// indentation. Both artifacts removed, content preserved.</summary>
    [Fact]
    public void CompoundSeam_FenceChurnPlusReindentedOverlap_IsCleaned()
    {
        string soFar = "```json\n{\n          {\n            \"id\": \"";
        string next = "```json\n            \"id\": \"obj_009\",\n            \"type\": \"ReturnTo\"";

        string cleaned = MemorySession.CleanResumeChunk(soFar, next);

        Assert.Equal("obj_009\",\n            \"type\": \"ReturnTo\"", cleaned);
        Assert.Contains("\"id\": \"obj_009\"", soFar + cleaned);
    }

    /// <summary>
    /// A capped answer with no visible text = the model deliberated through
    /// its entire budget (bit a live agent-to-agent test). One retry with
    /// double budget and think:false; accounting includes the burned round.
    /// </summary>
    [Fact]
    public void AllThinkingEmptyAnswer_IsRetriedWithDoubleBudget()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new StructuralSession(("   ", true), ("A real answer.", false));
        var ghost = new MemorySession(session,
            new ConversationMemory(store, s => s.Length / 4 + 1), 4096,
            s => s.Length / 4 + 1, maxAutoContinues: 3);

        var r = ghost.Generate(null, "hello", maxTokens: 200);

        Assert.Equal(2, session.Calls.Count);
        Assert.Equal(400u, session.Calls[1].MaxTokens);
        Assert.False(session.Calls[1].Think);
        Assert.Equal("A real answer.", r.Result.Text);
        Assert.False(r.Truncated);
        Assert.Equal(200u + 100u, r.Result.GeneratedTokens);   // burned + retry (400/4)
    }

    [Fact]
    public void EmptySalvage_StillEmpty_DoesNotLoop()
    {
        using var store = BlobStore.Open(StorePath());
        var session = new StructuralSession(("", true), ("", true));
        var ghost = new MemorySession(session,
            new ConversationMemory(store, s => s.Length / 4 + 1), 4096,
            s => s.Length / 4 + 1, maxAutoContinues: 3);

        var r = ghost.Generate(null, "hello");

        Assert.Equal(2, session.Calls.Count);   // main + one salvage, then stop
        Assert.False(r.Truncated);              // nothing to resume
    }
}
