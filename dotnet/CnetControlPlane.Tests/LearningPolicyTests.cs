using System.Security.Cryptography;
using System.Text;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

public sealed class LearningPolicyTests
{
    internal const string Valid = """
        {"schema_version":1,"enabled":true,"policy_id":"private_tables_v1",
         "datasets":[{"id":"calibration","authority":"verified_tool"}],
         "attempts_per_hour":4,"promotions_per_day":8,"max_attempts_per_source":2,
         "max_jobs":256,"max_storage_mib":256,"worker_seconds":30,
         "worker_memory_mib":512,"tick_seconds":30,"probation_probes":3,
         "max_probe_gap_seconds":120,"max_run_seconds":259200,"allocator_enabled":false}
        """;

    [Fact]
    public void ValidPolicyPinsExactBytesAndBounds()
    {
        var policy = LearningPolicy.Parse(Encoding.UTF8.GetBytes(Valid));
        Assert.True(policy.Enabled);
        Assert.Equal("private_tables_v1", policy.PolicyId);
        Assert.Equal(4, policy.AttemptsPerHour);
        Assert.Equal("verified_tool", Assert.Single(policy.Datasets).Authority);
        Assert.Equal(64, policy.Sha256.Length);
        Assert.NotEqual(policy.Sha256, LearningPolicy.Parse(Encoding.UTF8.GetBytes(Valid + "\n")).Sha256);
    }

    [Fact]
    public void PolicyAuthorityCanOnlyBeCreatedByValidatedFactory()
    {
        // Public construction or init setters admit forged hashes, unchecked
        // budgets, and a caller-owned mutable dataset list into the authority type.
        Assert.True(typeof(LearningPolicy).GetConstructors().Length == 0,
            "LEARNING_POLICY_RED public construction bypasses validated factory");
        Assert.All(typeof(LearningPolicy).GetProperties(), property =>
            Assert.True(property.SetMethod is null || property.SetMethod.IsPrivate,
                $"LEARNING_POLICY_RED public authority setter: {property.Name}"));
    }

    [Fact]
    public void ParsedDatasetAuthorityCannotBeMutated()
    {
        var policy = LearningPolicy.Parse(Encoding.UTF8.GetBytes(Valid));
        var datasets = Assert.IsAssignableFrom<IList<LearningDataset>>(policy.Datasets);
        Assert.True(datasets.IsReadOnly);
        Assert.Throws<NotSupportedException>(() => datasets.Add(new LearningDataset("injected", "user_correction")));
        Assert.Throws<NotSupportedException>(() => datasets[0] = new LearningDataset("calibration", "user_correction"));
        Assert.Equal("verified_tool", Assert.Single(policy.Datasets).Authority);
    }

    [Fact]
    public void CallerBufferMutationAfterReturnCannotChangeAuthorityOrHash()
    {
        var bytes = Encoding.UTF8.GetBytes(Valid);
        var expectedHash = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        var policy = LearningPolicy.Parse(bytes);
        Array.Fill(bytes, (byte)0xff);
        Assert.Equal(expectedHash, policy.Sha256);
        Assert.Equal("private_tables_v1", policy.PolicyId);
        Assert.True(policy.Enabled);
        Assert.Equal(4, policy.AttemptsPerHour);
        Assert.Equal(new LearningDataset("calibration", "verified_tool"), Assert.Single(policy.Datasets));
    }

    [Fact]
    public void ConcurrentCallerMutationNeverSeparatesAuthorityFromItsHash()
    {
        // Supplemental stress: scheduling is nondeterministic. A single changing
        // byte leaves every possible private snapshot valid and identifiable.
        var bytes = Encoding.UTF8.GetBytes(Valid);
        var changed = Encoding.UTF8.GetBytes(Valid.Replace("private_tables_v1", "qrivate_tables_v1"));
        var hashes = new Dictionary<string, string>
        {
            ["private_tables_v1"] = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(),
            ["qrivate_tables_v1"] = Convert.ToHexString(SHA256.HashData(changed)).ToLowerInvariant(),
        };
        var index = Valid.IndexOf("private_tables_v1", StringComparison.Ordinal);
        using var stop = new CancellationTokenSource();
        using var started = new ManualResetEventSlim();
        var writer = new Thread(() =>
        {
            started.Set();
            while (!stop.IsCancellationRequested)
            {
                bytes[index] = (byte)'q';
                Thread.Yield();
                bytes[index] = (byte)'p';
                Thread.Yield();
            }
        });
        writer.Start();
        try
        {
            started.Wait();
            for (var i = 0; i < 2000; i++)
            {
                var policy = LearningPolicy.Parse(bytes);
                Assert.True(hashes[policy.PolicyId] == policy.Sha256,
                    "LEARNING_POLICY_RED caller mutation separated authority from source hash");
            }
        }
        finally
        {
            stop.Cancel();
            writer.Join();
        }
    }

    [Theory]
    [InlineData("\"enabled\":true", "\"enabled\":true,\"enabled\":false")]
    [InlineData("\"enabled\":true,", "")]
    [InlineData("\"enabled\":true", "\"enabled\":1")]
    [InlineData("\"schema_version\":1", "\"schema_version\":2")]
    [InlineData("\"policy_id\":\"private_tables_v1\"", "\"policy_id\":\"../escape\"")]
    [InlineData("\"id\":\"calibration\"", "\"id\":\"Calibration\"")]
    [InlineData("\"authority\":\"verified_tool\"", "\"authority\":\"tier_a\"")]
    [InlineData("\"attempts_per_hour\":4", "\"attempts_per_hour\":0")]
    [InlineData("\"attempts_per_hour\":4", "\"attempts_per_hour\":33")]
    [InlineData("\"promotions_per_day\":8", "\"promotions_per_day\":31")]
    [InlineData("\"worker_seconds\":30", "\"worker_seconds\":121")]
    [InlineData("\"worker_memory_mib\":512", "\"worker_memory_mib\":2049")]
    [InlineData("\"max_run_seconds\":259200", "\"max_run_seconds\":259200.0")]
    [InlineData("\"max_probe_gap_seconds\":120", "\"max_probe_gap_seconds\":29")]
    [InlineData("\"allocator_enabled\":false", "\"allocator_enabled\":false,\"allow_shell\":true")]
    public void MalformedOrExpandedAuthorityRefuses(string from, string to) =>
        Assert.Throws<ArgumentException>(() => LearningPolicy.Parse(Encoding.UTF8.GetBytes(Valid.Replace(from, to))));

    [Fact]
    public void DuplicateDatasetsAndUnknownNestedFieldsRefuse()
    {
        const string item = "{\"id\":\"calibration\",\"authority\":\"verified_tool\"}";
        Assert.Throws<ArgumentException>(() => LearningPolicy.Parse(Encoding.UTF8.GetBytes(Valid.Replace(item, item + "," + item))));
        Assert.Throws<ArgumentException>(() => LearningPolicy.Parse(Encoding.UTF8.GetBytes(Valid.Replace("\"authority\":\"verified_tool\"", "\"authority\":\"verified_tool\",\"command\":\"anything\""))));
    }

    [Fact]
    public void DisabledPolicyParsesButCannotRun()
    {
        var policy = LearningPolicy.Parse(Encoding.UTF8.GetBytes(Valid.Replace("\"enabled\":true", "\"enabled\":false")));
        Assert.False(policy.Enabled);
        Assert.Throws<InvalidOperationException>(policy.RequireEnabled);
    }

    [Fact]
    public void MissingOversizedAndInvalidUtf8Refuse()
    {
        Assert.Throws<ArgumentException>(() => LearningPolicy.Parse([]));
        Assert.Throws<ArgumentException>(() => LearningPolicy.Parse(new byte[16385]));
        Assert.Throws<ArgumentException>(() => LearningPolicy.Parse([0xff, 0xfe]));
    }
}
