using System;
using System.IO;
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
        Assert.Equal(1.0, feat[0]); // calculator
        Assert.Equal(1.0, feat[6]); // expr
        Assert.Equal(1.0, feat[14]); // tool
    }

    [Theory]
    [InlineData(0, "calculator")]
    [InlineData(1, "memory_store")]
    [InlineData(2, "memory_recall")]
    [InlineData(3, "file_read")]
    [InlineData(4, "cnet_recall")]
    [InlineData(5, "final")]
    public void ExampleJson_Encodes_Expected_Tool_Features(int toolId, string toolName)
    {
        var feat = JsonToolCall.Encode(JsonToolCall.ExampleJson[toolId]);
        Assert.Equal(toolName, JsonToolCall.ToolNames[toolId]);
        // Each example includes either the tool name feature or final/answer.
        if (toolId < 5)
            Assert.Equal(1.0, feat[toolId]);
        else
            Assert.True(feat[5] > 0.5 || feat[13] > 0.5);
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
        Assert.Equal(16, JsonToolCall.FeatureNames.Length);
        Assert.Equal(JsonToolCall.FeatureCount, JsonToolCall.FeatureNames.Length);
        Assert.Equal(6, JsonToolCall.ToolNames.Length);
        Assert.Equal(JsonToolCall.ToolCount, JsonToolCall.ToolNames.Length);
        Assert.Equal(JsonToolCall.ToolCount, JsonToolCall.ExampleJson.Length);
    }

    [Fact]
    public void IsKnownTool_And_Normalize()
    {
        Assert.True(JsonToolCall.IsKnownTool("calculator"));
        Assert.True(JsonToolCall.IsKnownTool("finish"));
        Assert.False(JsonToolCall.IsKnownTool("web_search"));
        Assert.Equal("final", JsonToolCall.NormalizeTool("finish"));
        Assert.Equal("calculator", JsonToolCall.NormalizeTool("Calculator"));
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
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }
}
