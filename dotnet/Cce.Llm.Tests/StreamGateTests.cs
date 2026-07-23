using System.Text;
using CNET.Cce.Llm;
using Xunit;

namespace CNET.Cce.Llm.Tests;

/// <summary>
/// The stream gate: action scaffolding is withheld from the live stream, real
/// answers pass through with at most a few characters of buffering, and every
/// generation is classified independently.
/// </summary>
public sealed class StreamGateTests
{
    /// <summary>Feeds text one character at a time (the worst case for the
    /// buffering classifier) and returns what reached the viewer.</summary>
    private static string RunCharByChar(params string[] generations)
    {
        var sink = new StringBuilder();
        var gate = new StreamGate(s => sink.Append(s));
        foreach (string gen in generations)
        {
            gate.BeginGeneration();
            foreach (char c in gen) gate.Feed(c.ToString());
            gate.EndGeneration();
        }
        return sink.ToString();
    }

    [Fact]
    public void ActionGeneration_IsFullySuppressed()
    {
        Assert.Equal("", RunCharByChar("CALC: 738291 * 466517"));
        Assert.Equal("", RunCharByChar("RECALL: wifi password"));
        Assert.Equal("", RunCharByChar("READ: manual section"));
        Assert.Equal("", RunCharByChar("TOOL: area w=3, h=4"));
    }

    [Fact]
    public void NormalAnswer_StreamsInFull()
    {
        Assert.Equal("The answer is 42.", RunCharByChar("The answer is 42."));
    }

    [Fact]
    public void LeadingWhitespaceBeforeAction_IsStillSuppressed()
    {
        Assert.Equal("", RunCharByChar("\n  CALC: 2 + 2"));
    }

    [Fact]
    public void AnswerResemblingButNotAnAction_Streams()
    {
        // Case-sensitive, colon-terminated protocol: "Recall" prose is not RECALL:.
        Assert.Equal("Recall that we discussed this earlier.",
            RunCharByChar("Recall that we discussed this earlier."));
        Assert.Equal("READ the manual before you start.",
            RunCharByChar("READ the manual before you start."));
    }

    [Fact]
    public void ActionThenAnswer_OnlyTheAnswerStreams()
    {
        // The real deliberation shape: an action generation, then the answer.
        Assert.Equal("The area is 12.",
            RunCharByChar("CALC: 3 * 4", "The area is 12."));
    }

    [Fact]
    public void ChunkedDelivery_ClassifiesTheSameAsCharByChar()
    {
        var sink = new StringBuilder();
        var gate = new StreamGate(s => sink.Append(s));
        gate.BeginGeneration();
        gate.Feed("CA");         // ambiguous prefix — buffered
        gate.Feed("LC: 5 ");     // now a clear action
        gate.Feed("* 5");
        gate.EndGeneration();
        Assert.Equal("", sink.ToString());
    }

    [Fact]
    public void ShortAnswer_ThatPrefixesAnActionMarker_FlushesAtEnd()
    {
        // "RE" is a prefix of READ:/RECALL: but the generation ends there — it
        // is not an action, so EndGeneration flushes it as content.
        Assert.Equal("RE", RunCharByChar("RE"));
    }

    [Fact]
    public void AutoContinueChunks_EachStream()
    {
        // Multiple content generations (auto-continue) all reach the viewer.
        Assert.Equal("part one part two",
            RunCharByChar("part one", " part two"));
    }

    [Fact]
    public void SuppressCurrentGeneration_WithholdsTheWholeGeneration()
    {
        var sink = new System.Text.StringBuilder();
        var gate = new StreamGate(x => sink.Append(x));
        gate.BeginGeneration();
        gate.SuppressCurrentGeneration();           // resume round: suppress raw tokens
        gate.Feed("meant to ask");                  // the re-typed seam
        gate.EndGeneration();
        Assert.Equal("", sink.ToString());          // nothing streamed; the deduped
                                                    // chunk arrives via OnStitchedChunk
    }
}
