using System.Diagnostics;
using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningImportDurabilityTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    public LearningImportDurabilityTests(LearningCommandInstallation installation) { this.installation = installation; }
    private const string Source = "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 1\n7\t42\n";
    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    [Fact]
    public async Task FailedDirectorySyncAfterRenameRefusesRetainsExactEvidenceAndReleasesLocks()
    {
        using var deployment = installation.Deploy();
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        var bytes = Encoding.ASCII.GetBytes(Source);
        deployment.Put("approved.tsv", bytes);
        var source = Path.Combine(deployment.Root, "approved.tsv");
        var target = Path.Combine(deployment.Root, "work/data/calibration.tsv");
        var repository = LearningTestRepository.RequireBuilt(LearningCommandInstallation.NativeNames);
        var shim = Path.Combine(deployment.Root, "import-fsync-fault.so");
        var compiler = await LearningCommandInstallation.Execute("/usr/bin/cc",
            ["-B/usr/bin/", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
                Path.Combine(repository, "dotnet/CnetControlPlane.Tests/fixtures/learning_import_fsync_fault.c"), "-ldl", "-o", shim],
            repository, true);
        Assert.True(compiler.Code == 0, "fixed import fsync fixture failed to compile: " + compiler.Error);
        File.SetUnixFileMode(shim, UnixFileMode.UserRead);

        var refused = await ImportWithFault(deployment.Root, source, Hash(bytes), shim);
        Assert.Contains("LEARNING_IMPORT_DIRECTORY_FSYNC_INJECTED", refused.Error);
        Assert.True(refused.Code == 2, "LEARNING_IMPORT_DURABILITY_RED directory fsync failure claimed import success");
        Assert.Contains("learning_file_sync_uncertain", refused.Error);
        Assert.DoesNotContain("learning_source_imported", refused.Output);
        Assert.Equal(bytes, File.ReadAllBytes(target));
        Assert.Equal(UnixFileMode.UserRead | UnixFileMode.UserWrite, File.GetUnixFileMode(target));
        Assert.Equal([target], Directory.GetFileSystemEntries(Path.GetDirectoryName(target)!));

        using (var work = LearningFiles.Open(Path.Combine(deployment.Root, "work")))
        using (work.AcquireLock("owner.lock")) { }
        // Opening status performs a real SQLite write transaction, proving
        // the failed import released that lock as well as the owner lock.
        var status = await deployment.Command("status");
        Assert.Equal(0, status.Code);
        using (var observed = JsonDocument.Parse(status.Output))
        {
            Assert.False(observed.RootElement.GetProperty("paused").GetBoolean());
            Assert.Equal(0, observed.RootElement.GetProperty("jobs").GetInt64());
            Assert.Equal("not_started", observed.RootElement.GetProperty("run_state").GetString());
        }
        var retry = await deployment.Command("import", "calibration", source, Hash(bytes));
        Assert.Equal(2, retry.Code);
        Assert.Contains("learning_source_already_exists", retry.Error);
        // Even separately approved replacement bytes cannot overwrite the
        // evidence whose publication survived the uncertain directory sync.
        var replacement = Encoding.ASCII.GetBytes(Source.Replace("7\t42", "7\t43"));
        deployment.Put("approved.tsv", replacement);
        var overwrite = await deployment.Command("import", "calibration", source, Hash(replacement));
        Assert.Equal(2, overwrite.Code);
        Assert.Contains("learning_source_already_exists", overwrite.Error);
        Assert.Equal(bytes, File.ReadAllBytes(target));
    }

    private static async Task<(int Code, string Output, string Error)> ImportWithFault(
        string root, string source, string hash, string shim)
    {
        var start = new ProcessStartInfo(LearningCommandInstallation.Dotnet)
        {
            WorkingDirectory = root, UseShellExecute = false,
            RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true
        };
        // The trusted host/loader is outside managed installation attestation.
        // This one test-owned subprocess opts into the fixed fsync fault only.
        start.Environment.Clear();
        start.Environment["LD_PRELOAD"] = shim;
        foreach (var argument in new[] { Path.Combine(root, "managed/cnet-control.dll"), "learning", "import", root, "calibration", source, hash })
            start.ArgumentList.Add(argument);
        using var process = Process.Start(start)!;
        process.StandardInput.Close();
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        try { await process.WaitForExitAsync(timeout.Token); }
        catch { process.Kill(entireProcessTree: true); await process.WaitForExitAsync(); throw; }
        return (process.ExitCode, await output, await error);
    }
}
