using System.Runtime.Versioning;
using System.Text;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningTaskExecutionTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    public LearningTaskExecutionTests(LearningCommandInstallation installation) => this.installation = installation;

    [Theory]
    [InlineData("Could you kindly write 'µ' in caps!", 181)]
    [InlineData("Map the hyphen '-' to uppercase.", 45)]
    [InlineData("Go ahead and upcase 'µ'.", 181)]
    [InlineData("Would you mind putting 'µ' in uppercase?", 181)]
    [InlineData("Kindly recode the glyph 'µ' toward capitals.", 181)]
    [InlineData("Change this to uppercase: U+00B5", 181)]
    public async Task ConstituentProposalStillRequiresPolicyAndExternalEvidence(string request, byte originalByte)
    {
        Assert.True(LearningTaskParser.Propose(request).Status == "ready", "TASK_CONSTITUENT_RED execution route not recognized");
        using var deployment = installation.Deploy();
        var policyBytes = Encoding.UTF8.GetBytes(LearningPolicyTests.Valid.Replace("calibration", "unicode17_upper_latin1"));
        deployment.Put("policy.json", policyBytes);
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        var policy = LearningPolicy.Parse(policyBytes);
        using var ledger = LearningLedger.Open(Path.Combine(deployment.Root, "work"), policy, new LearningClock());
        using var native = LearningRuntime.Load(Path.Combine(deployment.Root, "native"), File.ReadAllBytes(Path.Combine(deployment.Root, "runtime.json")));
        int Execute(char id, string text) => LearningTaskCommand.Execute(ledger, native, policy,
            deployment.Root, "synthetic", new string(id, 32), text, new string('0', 32));
        Assert.Equal(0, Execute('a', "Could you kindly write 'É' in lowercase!"));
        Assert.Empty(ledger.Experiences(0, 100));
        await deployment.StartDaemon();
        Assert.Equal(0, Execute('b', request));
        Assert.Equal("awaiting_evidence", Assert.Single(ledger.Experiences(0, 100)).State);
        Assert.Equal((0L, 0L), ledger.Demand("unicode17_upper_latin1", originalByte));
        Assert.Equal(0, ledger.JobCount);
    }

    [Fact]
    public async Task InProcessExecutorPreservesPolicyDeduplicationAndUnknownTransportBoundaries()
    {
        using var deployment = installation.Deploy();
        var policyBytes = Encoding.UTF8.GetBytes(LearningPolicyTests.Valid.Replace("calibration", "unicode17_upper_latin1"));
        deployment.Put("policy.json", policyBytes);
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        var policy = LearningPolicy.Parse(policyBytes);
        using var ledger = LearningLedger.Open(Path.Combine(deployment.Root, "work"), policy, new LearningClock());
        using var native = LearningRuntime.Load(Path.Combine(deployment.Root, "native"), File.ReadAllBytes(Path.Combine(deployment.Root, "runtime.json")));
        int Execute(char id, string text) => LearningTaskCommand.Execute(ledger, native, policy,
            deployment.Root, "synthetic", new string(id, 32), text, new string('0', 32));
        Assert.Equal(0, Execute('a', "What is the lowercase of A?")); // Valid proposal, unauthorized dataset.
        Assert.Equal(0, Execute('b', "uppercase 65"));
        Assert.Empty(ledger.Experiences(0, 100));
        Assert.Equal(2, Execute('c', "uppercase µ")); // No socket: uncertainty is retained, not a learnable miss.
        Assert.Equal("unknown", Assert.Single(ledger.Experiences(0, 100)).State);
        await deployment.StartDaemon();
        Assert.Equal(2, Execute('c', "uppercase µ")); // Delivery replay cannot retry an unknown native call.
        Assert.Equal(0, Execute('d', "uppercase µ"));
        Assert.Equal("awaiting_evidence", ledger.Experiences(0, 100)[1].State);
        Assert.Equal((0L, 0L), ledger.Demand("unicode17_upper_latin1", 181));
        Assert.Equal(0, ledger.JobCount);
    }
}
