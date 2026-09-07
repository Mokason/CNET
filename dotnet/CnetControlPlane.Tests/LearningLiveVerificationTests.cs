using System.Runtime.Versioning;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningLiveVerificationTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    public LearningLiveVerificationTests(LearningCommandInstallation installation) { this.installation = installation; }

    [Fact]
    public async Task LiveVerificationChecksFullDomainWithoutCreatingDemandOrLearningWork()
    {
        using var deployment = installation.Deploy();
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        deployment.Put("work/data/calibration.tsv", Encoding.ASCII.GetBytes(
            "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 2\n0\t0\n7\t42\n"));
        await deployment.StartDaemon();
        var missing = await deployment.Command("verify", "calibration");
        Assert.True(missing.Code == 2 && missing.Output.Contains("learning_live_verification", StringComparison.Ordinal),
            "LEARNING_LIVE_VERIFY_RED missing bounded full-domain verification: " + missing.Error);
        using (var result = JsonDocument.Parse(missing.Output))
        {
            Assert.Equal(256, result.RootElement.GetProperty("checked_keys").GetInt32());
            Assert.Equal(2, result.RootElement.GetProperty("missing_answers").GetInt32());
            Assert.Equal(0, result.RootElement.GetProperty("wrong_answers").GetInt32());
        }
        var idle = await deployment.Command("tick");
        Assert.Equal(0, idle.Code);
        using (var result = JsonDocument.Parse(idle.Output)) Assert.Equal("idle", result.RootElement.GetProperty("action").GetString());
        Assert.Equal(0, (await deployment.Command("ask", "calibration", "7")).Code);
        Assert.Equal(0, (await deployment.Command("tick")).Code);
        var verified = await deployment.Command("verify", "calibration");
        Assert.True(verified.Code == 0, "LEARNING_LIVE_VERIFY_RED learned full-domain check: " + verified.Error);
        using (var result = JsonDocument.Parse(verified.Output))
        {
            Assert.True(result.RootElement.GetProperty("passed").GetBoolean());
            Assert.Equal(2, result.RootElement.GetProperty("correct_answers").GetInt32());
            Assert.Equal(254, result.RootElement.GetProperty("correct_abstentions").GetInt32());
            Assert.Equal(0, result.RootElement.GetProperty("wrong_answers").GetInt32());
            Assert.Equal(0, result.RootElement.GetProperty("missing_answers").GetInt32());
            Assert.Equal(64, result.RootElement.GetProperty("source_sha256").GetString()!.Length);
            Assert.Equal(64, result.RootElement.GetProperty("active_sha256").GetString()!.Length);
            Assert.Equal(2UL, result.RootElement.GetProperty("revision").GetUInt64());
        }
        deployment.Put("work/data/calibration.tsv", Encoding.ASCII.GetBytes(
            "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 2\n0\t0\n7\t43\n"));
        var stale = await deployment.Command("verify", "calibration");
        Assert.Equal(2, stale.Code);
        using (var result = JsonDocument.Parse(stale.Output))
        {
            Assert.Equal(2, result.RootElement.GetProperty("missing_answers").GetInt32());
            Assert.Equal(0, result.RootElement.GetProperty("wrong_answers").GetInt32());
        }
        Assert.Equal(2, (await deployment.Command("verify", "unauthorized")).Code);
        using var status = JsonDocument.Parse((await deployment.Command("status")).Output);
        Assert.Equal(1, status.RootElement.GetProperty("jobs").GetInt64());
    }
}

public sealed class LearningLiveObservationTests
{
    private static readonly LearningDataset Dataset = new("calibration", "verified_tool");
    private static readonly ControlStatus Healthy = new(true, 2, new string('a', 64), null, null, true, ControlReason.Ok);
    private static LocalTableReference Source(ushort value = 42) => LocalTableReference.Parse(Encoding.ASCII.GetBytes(
        "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 2\n0\t0\n7\t" + value + "\n"), Dataset);
    private static LearningAskResult Answer(ushort? value) => LearningAskResult.Parse(JsonSerializer.SerializeToUtf8Bytes(new
    {
        ok = true, teacher = false, verified = value.HasValue, miss = !value.HasValue,
        source = value.HasValue ? "LOCAL" : "CNET", skill = value.HasValue ? "capsule_core" : "capsule_refusal",
        answer = value.HasValue ? value.Value.ToString(System.Globalization.CultureInfo.InvariantCulture) : "ABSTAIN: uncovered"
    }).Concat(new[] { (byte)'\n' }).ToArray());
    private sealed class Clock : ILearningClock
    {
        internal LearningInstant Current = new("boot", 100);
        public LearningInstant Now => Current;
    }

    [Theory]
    [InlineData("wrong", 1, 0)]
    [InlineData("extra", 1, 0)]
    [InlineData("missing", 0, 1)]
    [InlineData("zero", 0, 0)]
    public async Task WrongExtraAndMissingAnswersNeverPassAndZeroIsNotAbstention(string fault, int wrong, int missing)
    {
        var reference = Source(); var keys = new List<byte>();
        var result = await LearningLiveVerification.ObserveAsync(Dataset, () => reference, _ => Task.FromResult(Healthy), (key, _) =>
        {
            keys.Add(key);
            var value = reference.ExpectedFor(key);
            if (fault == "wrong" && key == 7) value = 43;
            if (fault == "extra" && key == 255) value = 0;
            if (fault == "missing" && key == 0) value = null;
            return Task.FromResult(Answer(value));
        }, 2, new Clock());
        Assert.Equal(Enumerable.Range(0, 256).Select(k => (byte)k), keys);
        Assert.Equal(wrong, result.WrongAnswers); Assert.Equal(missing, result.MissingAnswers);
        Assert.Equal(fault == "zero", result.Passed);
        Assert.Equal(256, result.CorrectAnswers + result.CorrectAbstentions + result.WrongAnswers + result.MissingAnswers);
    }

    [Theory]
    [InlineData("revision")]
    [InlineData("active")]
    [InlineData("staged")]
    [InlineData("durability")]
    [InlineData("source")]
    [InlineData("reboot")]
    [InlineData("backwards")]
    [InlineData("suspend")]
    public async Task MidSweepIdentityAndClockChangesCannotYieldSuccess(string change)
    {
        var clock = new Clock(); var source = Source(); var status = Healthy;
        await Assert.ThrowsAsync<InvalidOperationException>(() => LearningLiveVerification.ObserveAsync(Dataset,
            () => source, _ => Task.FromResult(status), (key, _) =>
            {
                var answer = Answer(source.ExpectedFor(key));
                if (key == 128)
                {
                    if (change == "revision") status = status with { Revision = 3 };
                    if (change == "active") status = status with { Active = new string('b', 64) };
                    if (change == "staged") status = status with { Staged = new string('c', 64) };
                    if (change == "durability") status = status with { Durable = false };
                    if (change == "source") source = Source(43);
                    if (change == "reboot") clock.Current = new("newboot", 101);
                    if (change == "backwards") clock.Current = new("boot", 99);
                    if (change == "suspend") clock.Current = new("boot", 2_000_000_100);
                }
                return Task.FromResult(answer);
            }, 2, clock));
    }

    [Fact]
    public async Task ClockIsCheckedAfterFinalSourceReadAndPreCancellationPreventsWork()
    {
        var clock = new Clock(); var reads = 0; var reference = Source();
        await Assert.ThrowsAsync<InvalidOperationException>(() => LearningLiveVerification.ObserveAsync(Dataset, () =>
        {
            if (++reads == 2) clock.Current = new("boot", 2_000_000_100);
            return reference;
        }, _ => Task.FromResult(Healthy), (key, _) => Task.FromResult(Answer(reference.ExpectedFor(key))), 2, clock));
        using var cancelled = new CancellationTokenSource(); cancelled.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => LearningLiveVerification.ObserveAsync(Dataset,
            () => throw new Exception("must not read source"), _ => Task.FromResult(Healthy),
            (_, _) => throw new Exception("must not ask"), 2, new Clock(), cancelled.Token));
    }

    [Theory]
    [InlineData("ask")]
    [InlineData("final_source")]
    public async Task CallerCancellationDuringObservationNeverYieldsAReceipt(string pending)
    {
        var reference = Source(); var reads = 0;
        using var cancelled = new CancellationTokenSource();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => LearningLiveVerification.ObserveAsync(Dataset, () =>
        {
            if (++reads == 2 && pending == "final_source") cancelled.Cancel();
            return reference;
        }, _ => Task.FromResult(Healthy), async (key, stop) =>
        {
            if (pending == "ask") { cancelled.Cancel(); await Task.Delay(Timeout.Infinite, stop); }
            return Answer(reference.ExpectedFor(key));
        }, 2, new Clock(), cancelled.Token));
    }

    [Theory]
    [InlineData("", 100)]
    [InlineData("boot", -1)]
    public async Task InvalidInitialClockRefusesBeforeSourceOrTransport(string boot, long time)
    {
        await Assert.ThrowsAsync<InvalidOperationException>(() => LearningLiveVerification.ObserveAsync(Dataset,
            () => throw new Exception("must not read"), _ => throw new Exception("must not inspect native"),
            (_, _) => throw new Exception("must not ask"), 2, new Clock { Current = new(boot, time) }));
    }

    [Fact]
    public async Task WholeSweepHasOneDeadlineRatherThanANewBudgetForEachKey()
    {
        var reference = Source(); var calls = 0;
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => LearningLiveVerification.ObserveAsync(Dataset,
            () => reference, _ => Task.FromResult(Healthy), async (key, stop) =>
            {
                calls++; await Task.Delay(200, stop);
                return Answer(reference.ExpectedFor(key));
            }, 1, new Clock()).WaitAsync(TimeSpan.FromSeconds(4)));
        Assert.InRange(calls, 1, 10);
    }

    [Theory]
    [InlineData("ask")]
    [InlineData("status")]
    public async Task GlobalSuspendDeadlineCancelsAnAlreadyPendingExchange(string pending)
    {
        var clock = new Clock(); var reference = Source();
        using var cancelled = new CancellationTokenSource();
        async Task<T> Suspended<T>(CancellationToken stop)
        {
            clock.Current = new("boot", 120_000_000_100);
            await Task.Delay(Timeout.Infinite, stop);
            throw new Exception("LEARNING_LIVE_DEADLINE_RED suspended exchange survived cancellation");
        }
        var observation = LearningLiveVerification.ObserveAsync(Dataset, () => reference,
            stop => pending == "status" ? Suspended<ControlStatus>(stop) : Task.FromResult(Healthy),
            (_, stop) => Suspended<LearningAskResult>(stop), 120, clock, cancelled.Token);
        try
        {
            var error = await Assert.ThrowsAnyAsync<Exception>(() => observation.WaitAsync(TimeSpan.FromSeconds(1)));
            Assert.True(error is InvalidOperationException or OperationCanceledException,
                "LEARNING_LIVE_DEADLINE_RED overall boot deadline failed to cancel pending " + pending);
        }
        finally
        {
            cancelled.Cancel();
            try { await observation; }
            catch (Exception error) when (error is InvalidOperationException or OperationCanceledException) { }
        }
    }

    [Theory]
    [InlineData("uncertain")]
    [InlineData("staged")]
    [InlineData("empty")]
    [InlineData("refused")]
    public async Task UnhealthyNativeStateRefusesBeforeSendingProbes(string fault)
    {
        var status = fault switch
        {
            "uncertain" => Healthy with { Durable = false },
            "staged" => Healthy with { Staged = new string('b', 64) },
            "empty" => Healthy with { Active = null },
            _ => Healthy with { Ok = false, Reason = ControlReason.Refused }
        };
        await Assert.ThrowsAsync<InvalidOperationException>(() => LearningLiveVerification.ObserveAsync(Dataset,
            () => Source(), _ => Task.FromResult(status), (_, _) => throw new Exception("must not ask"), 2, new Clock()));
    }
}
