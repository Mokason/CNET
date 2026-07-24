using System;
using System.IO;
using System.Linq;
using Xunit;
using CNET.Cce;

namespace CNET.Cce.Tests;

/// <summary>
/// Host-side closed-set JSON tool-call bridge (no native soul required).
/// Native seal/serve is covered by make json_toolcall.
/// </summary>
public class JsonToolCallTests
{
    [Fact]
    public void Encode_Detects_Tool_Keywords()
    {
        var feat = JsonToolCall.Encode("""{"tool":"calculator","args":{"expr":"1+2"}}""");
        Assert.Equal(JsonToolCall.FeatureCount, feat.Length);
        Assert.Equal(1.0, feat[Array.IndexOf(JsonToolCall.FeatureNames, "calculator")]);
        Assert.Equal(1.0, feat[Array.IndexOf(JsonToolCall.FeatureNames, "expr")]);
        Assert.Equal(1.0, feat[Array.IndexOf(JsonToolCall.FeatureNames, "tool")]);
    }

    [Theory]
    [InlineData(0, "calculator")]
    [InlineData(1, "memory_store")]
    [InlineData(2, "memory_recall")]
    [InlineData(3, "file_read")]
    [InlineData(4, "cnet_recall")]
    [InlineData(5, "web_search")]
    [InlineData(6, "wiki_lookup")]
    [InlineData(7, "final")]
    public void ExampleJson_Encodes_Expected_Tool_Features(int toolId, string toolName)
    {
        var feat = JsonToolCall.Encode(JsonToolCall.ExampleJson[toolId]);
        Assert.Equal(toolName, JsonToolCall.ToolNames[toolId]);
        int fi = Array.IndexOf(JsonToolCall.FeatureNames, toolName);
        if (toolName != "final")
            Assert.Equal(1.0, feat[fi]);
        else
            Assert.True(feat[fi] > 0.5 ||
                        feat[Array.IndexOf(JsonToolCall.FeatureNames, "answer")] > 0.5);
    }

    [Fact]
    public void DecodeTool_Argmax()
    {
        var onehot = new double[JsonToolCall.ToolCount];
        onehot[3] = 1.0;
        Assert.Equal("file_read", JsonToolCall.DecodeTool(onehot));
        Assert.Equal(3, JsonToolCall.DecodeToolId(onehot));
    }

    [Fact]
    public void TryParseAgentJson_Tool_And_Final()
    {
        Assert.True(JsonToolCall.TryParseAgentJson(
            """{"thought":"t","tool":"calculator","args":{"expr":"2+2"}}""",
            out var tool, out var args, out var final));
        Assert.Equal("calculator", tool);
        Assert.Null(final);
        Assert.Equal("2+2", args.GetProperty("expr").GetString());

        Assert.True(JsonToolCall.TryParseAgentJson(
            """{"thought":"done","final":"42"}""",
            out tool, out _, out final));
        Assert.Equal("final", tool);
        Assert.Equal("42", final);
    }

    [Fact]
    public void TryParseAgentJson_Rejects_Garbage()
    {
        Assert.False(JsonToolCall.TryParseAgentJson("not json", out _, out _, out _));
    }

    [Fact]
    public void Feature_Alphabet_Length_Matches_Native_Contract()
    {
        Assert.Equal(18, JsonToolCall.FeatureNames.Length);
        Assert.Equal(JsonToolCall.FeatureCount, JsonToolCall.FeatureNames.Length);
        Assert.Equal(8, JsonToolCall.ToolNames.Length);
        Assert.Equal(JsonToolCall.ToolCount, JsonToolCall.ToolNames.Length);
        Assert.Equal(JsonToolCall.ToolCount, JsonToolCall.ExampleJson.Length);
        Assert.Contains("web_search", JsonToolCall.ToolNames);
        Assert.Contains("wiki_lookup", JsonToolCall.ToolNames);
        Assert.Equal("json_toolcall_v2", JsonToolCall.UnitName);
    }

    [Fact]
    public void IsKnownTool_And_Normalize()
    {
        Assert.True(JsonToolCall.IsKnownTool("calculator"));
        Assert.True(JsonToolCall.IsKnownTool("web_search"));
        Assert.True(JsonToolCall.IsKnownTool("wiki_lookup"));
        Assert.True(JsonToolCall.IsKnownTool("finish"));
        Assert.False(JsonToolCall.IsKnownTool("browser"));
        Assert.Equal("final", JsonToolCall.NormalizeTool("finish"));
        Assert.Equal("calculator", JsonToolCall.NormalizeTool("Calculator"));
        Assert.Equal("web_search", JsonToolCall.NormalizeTool("Web_Search"));
    }

    [Fact]
    public void NoteGap_Writes_Inbox_Line()
    {
        var path = Path.Combine(Path.GetTempPath(), "cnet_jtc_gap_test.inbox");
        try
        {
            if (File.Exists(path)) File.Delete(path);
            Assert.True(JsonToolCall.NoteGap(path));
            var line = File.ReadAllText(path);
            Assert.Contains("NO_PLAN", line);
            Assert.Contains("jtc_feat", line);
            Assert.Contains("json_tool", line);
            Assert.Contains($" {JsonToolCall.FeatureCount} ", line);
            Assert.Contains($" {JsonToolCall.ToolCount} ", line);
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    /// <summary>The host-side recorded-traffic capture: LogTraffic appends each
    /// request as single-line JSONL to CNET_JTC_TRAFFIC_LOG, replayable through
    /// Encode. When the env is set externally, produce a larger host log at that
    /// path and leave it (so the C personal-AI replay can consume it); otherwise
    /// self-contained with a temp file.</summary>
    [Fact]
    public void LogTraffic_ProducesReplayableHostLog()
    {
        string? external = Environment.GetEnvironmentVariable("CNET_JTC_TRAFFIC_LOG");
        bool demo = !string.IsNullOrEmpty(external);
        string path = demo ? external! : Path.Combine(Path.GetTempPath(), $"jtc_traffic_{Guid.NewGuid():N}.jsonl");
        if (!demo) Environment.SetEnvironmentVariable("CNET_JTC_TRAFFIC_LOG", path);
        if (File.Exists(path)) File.Delete(path);
        try
        {
            int n = demo ? 2000 : 80;
            var rng = new Random(20260724);
            for (int i = 0; i < n; i++)
                JsonToolCall.LogTraffic(GenRequest(rng));      // the host capture path
            var lines = File.ReadAllLines(path);
            Assert.True(lines.Length >= n * 9 / 10, $"expected ~{n}, got {lines.Length}");
            Assert.All(lines, l => Assert.DoesNotContain('\n', l));                 // single-line JSONL
            Assert.All(lines, l => Assert.Contains(JsonToolCall.Encode(l), x => x != 0.0)); // encodable
        }
        finally
        {
            if (!demo)
            {
                Environment.SetEnvironmentVariable("CNET_JTC_TRAFFIC_LOG", null);
                if (File.Exists(path)) File.Delete(path);
            }
            // demo mode: leave the file as the replayable host log
        }
    }

    // Realistic tool-call request shapes (clean / underspecified / ambiguous),
    // mirroring the personal-AI replay harness so a produced host log exercises
    // the same fault distribution.
    private static string GenRequest(Random rng)
    {
        string[] tool = JsonToolCall.ToolNames;
        string[] args =
        {
            "\"expr\":\"n\"", "\"key\":\"k\",\"value\":\"v\"", "\"query\":\"q\"",
            "\"path\":\"f.txt\"", "\"cond\":0,\"current\":1", "\"query\":\"web thing\"",
            "\"query\":\"topic\"", "\"answer\":\"ok\""
        };
        int t = rng.Next(8), style = rng.Next(10);
        if (t == 7) return "{\"final\":\"stop\"," + args[7] + "}";
        if (style < 6) return "{\"tool\":\"" + tool[t] + "\",\"args\":{" + args[t] + "}}";
        if (style < 8) return "{\"args\":{" + args[t] + "}}";
        int t2 = rng.Next(7);
        return "{\"tool\":\"" + tool[t] + "\",\"args\":{" + args[t] + ",\"note\":\"see " + tool[t2] + "\"}}";
    }
}
