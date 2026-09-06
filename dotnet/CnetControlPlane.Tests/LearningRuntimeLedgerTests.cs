using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

public sealed class LearningRuntimeLedgerTests : IDisposable
{
    private readonly string root = Directory.CreateTempSubdirectory("cnet-runtime-pin-").FullName;
    private const UnixFileMode Private = UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute;
    private readonly string[] names = ["cnet_table_capsule", "cnet_table_verify", "cnet_learning_snapshot", "cnet_capsulectl", "cnetd", "libcnet_capsule_core.so"];
    private static LearningPolicy Policy => LearningPolicy.Parse(Encoding.UTF8.GetBytes(LearningPolicyTests.Valid));
    private sealed class Clock : ILearningClock
    {
        public LearningInstant Now => new("00000000-0000-0000-0000-000000000001", 100_000_000_000);
    }
    public LearningRuntimeLedgerTests()
    {
        File.SetUnixFileMode(root, Private);
        Directory.CreateDirectory(Path.Combine(root, "runtime"), Private);
        Directory.CreateDirectory(Path.Combine(root, "ledger"), Private);
    }
    public void Dispose() => Directory.Delete(root, true);
    private LearningRuntime Runtime(byte identity)
    {
        var hashes = new Dictionary<string, string>();
        foreach (var name in names)
        {
            var path = Path.Combine(root, "runtime", name);
            if (File.Exists(path)) File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
            // Byte-attestation unit fixtures only; never executed as programs.
            File.WriteAllBytes(path, [identity]);
            File.SetUnixFileMode(path, name.EndsWith(".so", StringComparison.Ordinal)
                ? UnixFileMode.UserRead : UnixFileMode.UserRead | UnixFileMode.UserExecute);
            hashes.Add(name, Convert.ToHexString(SHA256.HashData([identity])).ToLowerInvariant());
        }
        return LearningRuntime.Load(Path.Combine(root, "runtime"), JsonSerializer.SerializeToUtf8Bytes(new { schema_version = 1, files = hashes }));
    }

    [Fact]
    public void ExactVerifiedRuntimeIdentitySurvivesRestart()
    {
        using var runtime = Runtime(1);
        using (var ledger = LearningLedger.Create(Path.Combine(root, "ledger"), Policy, new Clock()))
        {
            Assert.Null(ledger.RuntimeSha256);
            ledger.BindRuntime(runtime);
            Assert.Equal(runtime.Sha256, ledger.RuntimeSha256);
        }
        using var reopened = LearningLedger.Open(Path.Combine(root, "ledger"), Policy, new Clock());
        reopened.BindRuntime(runtime);
        Assert.Equal(runtime.Sha256, reopened.RuntimeSha256);
    }

    [Fact]
    public void DifferentValidRuntimeDurablyPausesInsteadOfReplacingIdentity()
    {
        string original;
        using (var runtime = Runtime(1))
        using (var ledger = LearningLedger.Create(Path.Combine(root, "ledger"), Policy, new Clock()))
        {
            ledger.BindRuntime(runtime); original = runtime.Sha256;
        }
        using var replacement = Runtime(2);
        using (var ledger = LearningLedger.Open(Path.Combine(root, "ledger"), Policy, new Clock()))
            Assert.Equal("learning_runtime_identity_changed", Assert.Throws<InvalidOperationException>(() => ledger.BindRuntime(replacement)).Message);
        using var reopened = LearningLedger.Open(Path.Combine(root, "ledger"), Policy, new Clock());
        Assert.Equal(original, reopened.RuntimeSha256);
        Assert.Equal("learning_paused", reopened.Reserve("calibration", new string('a', 64)).Reason);
    }

    [Fact]
    public void ChangedFilesCannotBeBoundThroughAnOldAttestationObject()
    {
        using var old = Runtime(1);
        using var changed = Runtime(2);
        using var ledger = LearningLedger.Create(Path.Combine(root, "ledger"), Policy, new Clock());
        Assert.Throws<InvalidOperationException>(() => ledger.BindRuntime(old));
        Assert.Null(ledger.RuntimeSha256);
    }

    [Fact]
    public void FirstRuntimeCannotBeInventedAfterUnpinnedWorkWasReserved()
    {
        using var runtime = Runtime(1);
        using var ledger = LearningLedger.Create(Path.Combine(root, "ledger"), Policy, new Clock());
        Assert.NotNull(ledger.Reserve("calibration", new string('a', 64)).Job);
        Assert.Equal("learning_runtime_unbound_jobs", Assert.Throws<InvalidOperationException>(() => ledger.BindRuntime(runtime)).Message);
        Assert.Null(ledger.RuntimeSha256);
        Assert.Equal(1, ledger.JobCount);
    }
}
