using System.Runtime.Versioning;
using System.Text.Json;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningTraceCommandTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    public LearningTraceCommandTests(LearningCommandInstallation installation) => this.installation = installation;

    [Fact]
    public async Task ExactBatchTraceRetainsMissingIdsAndReadsWithoutChangingEitherSchemaOrBytes()
    {
        using var deployment = installation.Deploy();
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        await deployment.StartDaemon();
        var ids = Enumerable.Range(1, 10).Select(value => value.ToString("x32")).ToArray();
        foreach (var id in ids.Take(9))
            Assert.Equal(0, (await deployment.Command("observe", "calibration", "7", "synthetic", id)).Code);
        Array.Reverse(ids);
        var path = Path.Combine(deployment.Root, "work/ledger.sqlite");
        var before = File.ReadAllBytes(path);
        var response = await deployment.Command("trace", string.Join(',', ids));
        Assert.True(response.Code == 0, "TASK_TRACE_RED exact bounded history lookup missing: " + response.Error);
        Assert.True(System.Text.Encoding.UTF8.GetByteCount(response.Output) < 32768);
        using var result = JsonDocument.Parse(response.Output);
        Assert.Equal("learning_trace", result.RootElement.GetProperty("event").GetString());
        var matches = result.RootElement.GetProperty("matches").EnumerateArray().ToArray();
        Assert.Equal(ids, matches.Select(row => row.GetProperty("RequestId").GetString()));
        Assert.Equal(JsonValueKind.Null, matches[0].GetProperty("Experience").ValueKind);
        Assert.All(matches.Skip(1), row => Assert.Equal(row.GetProperty("RequestId").GetString(),
            row.GetProperty("Experience").GetProperty("RequestId").GetString()));
        Assert.Equal(before, File.ReadAllBytes(path));
        foreach (var input in new[] { "", ids[0] + "," + ids[0], string.Join(',', ids) + "," + new string('f', 32),
                                      new string('F', 32), "bad-id", ids[0] + "," })
            Assert.Equal(2, (await deployment.Command("trace", input)).Code);
        Assert.Equal(before, File.ReadAllBytes(path));
    }
}
