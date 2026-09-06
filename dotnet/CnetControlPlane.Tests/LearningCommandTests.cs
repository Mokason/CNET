using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CnetControlPlane.Learning;
using Xunit;

namespace CnetControlPlane.Tests;

[SupportedOSPlatform("linux")]
public sealed class LearningCommandTests : IClassFixture<LearningCommandInstallation>
{
    private readonly LearningCommandInstallation installation;
    public LearningCommandTests(LearningCommandInstallation installation) { this.installation = installation; }

    [Fact]
    public async Task RealPrivateEntryAssemblyBindsPackagedSqliteBeforeUse()
    {
        using var deployment = installation.Deploy();
        var result = await deployment.Command("inspect");
        Assert.True(result.Code == 0, "LEARNING_COMMAND_RED real managed binding: " + result.Error);
        using var json = JsonDocument.Parse(result.Output);
        Assert.Equal("learning_inspected", json.RootElement.GetProperty("event").GetString());
        Assert.Equal(deployment.ManagedHash, json.RootElement.GetProperty("managed_sha256").GetString());
        Assert.Equal("3.53.4", json.RootElement.GetProperty("sqlite_version").GetString());
        Assert.False(Directory.Exists(Path.Combine(deployment.Root, "work")));
    }

    [Fact]
    public async Task InitializePinsBothInstallationsAndNeverResetsAnExistingLedger()
    {
        using var deployment = installation.Deploy();
        var first = await deployment.Command("initialize");
        Assert.True(first.Code == 0, "LEARNING_COMMAND_RED initialize: " + first.Error);
        var status = await deployment.Command("status");
        Assert.Equal(0, status.Code);
        using var json = JsonDocument.Parse(status.Output);
        Assert.Equal(deployment.ManagedHash, json.RootElement.GetProperty("managed_sha256").GetString());
        Assert.Equal("not_started", json.RootElement.GetProperty("run_state").GetString());
        Assert.Equal(0, json.RootElement.GetProperty("jobs").GetInt64());
        Assert.NotEqual(0, (await deployment.Command("initialize")).Code);
        Assert.Equal(0, (await deployment.Command("status")).Code);
    }

    [Fact]
    public async Task ChangedManagedManifestCannotRebindAnInitializedLedger()
    {
        using var deployment = installation.Deploy();
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        var path = Path.Combine(deployment.Root, "managed.json");
        deployment.Put("managed.json", [.. File.ReadAllBytes(path), (byte)'\n']);
        var refused = await deployment.Command("status");
        Assert.Equal(2, refused.Code);
        Assert.Contains("learning_managed_identity_changed", refused.Error);
        var policy = LearningPolicy.Parse(File.ReadAllBytes(Path.Combine(deployment.Root, "policy.json")));
        using var ledger = LearningLedger.Open(Path.Combine(deployment.Root, "work"), policy, new LearningClock());
        Assert.True(ledger.IsPaused);
        Assert.Equal(deployment.ManagedHash, ledger.ManagedSha256);
    }

    [Theory]
    [InlineData("managed_binding")]
    [InlineData("runtime_binding")]
    public async Task MissingInitializedPinCannotBeRecreatedByStatus(string table)
    {
        using var deployment = installation.Deploy();
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        var builder = new Microsoft.Data.Sqlite.SqliteConnectionStringBuilder
            { DataSource = Path.Combine(deployment.Root, "work/ledger.sqlite"), Pooling = false };
        using (var connection = new Microsoft.Data.Sqlite.SqliteConnection(builder.ToString()))
        {
            connection.Open(); using var query = connection.CreateCommand();
            query.CommandText = "DELETE FROM " + table; query.ExecuteNonQuery(); // Fixed theory values, test fault only.
        }
        var result = await deployment.Command("status");
        Assert.True(result.Code == 2, "LEARNING_PIN_RED initialized missing pin was recreated");
        Assert.Contains("learning_installation_binding_missing", result.Error);
        var policy = LearningPolicy.Parse(File.ReadAllBytes(Path.Combine(deployment.Root, "policy.json")));
        using var ledger = LearningLedger.Open(Path.Combine(deployment.Root, "work"), policy, new LearningClock());
        Assert.True(ledger.IsPaused);
        Assert.Null(table == "managed_binding" ? ledger.ManagedSha256 : ledger.RuntimeSha256);
    }

    [Theory]
    [InlineData("managed/Microsoft.Data.Sqlite.dll")]
    [InlineData("managed/runtimes/linux-x64/native/libe_sqlite3.so")]
    [InlineData("native/cnet_table_verify")]
    public async Task ChangedPinnedCodeRefusesBeforeCreatingAnyWork(string relative)
    {
        using var deployment = installation.Deploy();
        var path = Path.Combine(deployment.Root, relative);
        File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        using (var file = File.Open(path, FileMode.Open, FileAccess.Write)) { file.Position = file.Length - 1; file.WriteByte(0xa5); }
        File.SetUnixFileMode(path, UnixFileMode.UserRead | (relative.StartsWith("native/", StringComparison.Ordinal) ? UnixFileMode.UserExecute : 0));
        Assert.NotEqual(0, (await deployment.Command("initialize")).Code);
        Assert.False(Directory.Exists(Path.Combine(deployment.Root, "work")));
    }

    [Fact]
    public async Task OperatorPauseIsDurableWithoutRequiringAHealthyDaemon()
    {
        using var deployment = installation.Deploy();
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        var pause = await deployment.Command("pause");
        Assert.True(pause.Code == 0, "LEARNING_OPERATOR_RED pause: " + pause.Error);
        using var json = JsonDocument.Parse((await deployment.Command("status")).Output);
        Assert.True(json.RootElement.GetProperty("paused").GetBoolean());
        Assert.Equal(2, (await deployment.Command("resume")).Code);
    }

    [Fact]
    public async Task ActualPrivateDaemonAnswersDemandAndPerformsATypedLearningTick()
    {
        using var deployment = installation.Deploy();
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        deployment.Put("work/data/calibration.tsv", Encoding.ASCII.GetBytes(
            "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 1\n7\t42\n"));
        await deployment.StartDaemon();
        var ask = await deployment.Command("ask", "calibration", "7");
        Assert.True(ask.Code == 0, "LEARNING_OPERATOR_RED ask: " + ask.Error);
        using (var before = JsonDocument.Parse(ask.Output)) Assert.False(before.RootElement.GetProperty("verified").GetBoolean());
        var tick = await deployment.Command("tick");
        Assert.True(tick.Code == 0, "LEARNING_OPERATOR_RED tick: " + tick.Error);
        using (var action = JsonDocument.Parse(tick.Output)) Assert.Equal("activated", action.RootElement.GetProperty("action").GetString());
        var learned = await deployment.Command("ask", "calibration", "7");
        Assert.Equal(0, learned.Code);
        using var after = JsonDocument.Parse(learned.Output);
        Assert.True(after.RootElement.GetProperty("verified").GetBoolean());
        Assert.Equal(42, after.RootElement.GetProperty("value").GetInt32());
        Assert.Equal(2, (await deployment.Command("ask", "calibration", "007")).Code);
        Assert.Equal(2, (await deployment.Command("ask", "unauthorized", "7")).Code);
        Assert.Equal(0, (await deployment.Command("pause")).Code);
        Assert.Equal(0, (await deployment.Command("resume")).Code);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ExplicitQuiesceRollsBackAndCanRepeatWithoutDemandAdmission(bool missingAsk)
    {
        using var deployment = installation.Deploy();
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        deployment.Put("work/data/calibration.tsv", Encoding.ASCII.GetBytes(
            "CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows 1\n7\t42\n"));
        await deployment.StartDaemon();
        Assert.Equal(0, (await deployment.Command("ask", "calibration", "7")).Code);
        Assert.Equal(0, (await deployment.Command("tick")).Code);
        if (missingAsk) File.Delete(Path.Combine(deployment.Root, "ipc/ask.sock"));
        var result = await deployment.Command("quiesce");
        Assert.True(result.Code == 0, "LEARNING_QUIESCE_COMMAND_RED cleanup: " + result.Error);
        using (var status = JsonDocument.Parse(result.Output))
        {
            Assert.Equal("rolled_back", status.RootElement.GetProperty("action").GetString());
            Assert.True(status.RootElement.GetProperty("paused").GetBoolean());
            Assert.Equal("not_started", status.RootElement.GetProperty("run_state").GetString());
            Assert.Equal(JsonValueKind.Null, status.RootElement.GetProperty("pending_intent").ValueKind);
            Assert.Equal(JsonValueKind.Null, status.RootElement.GetProperty("outstanding_state").ValueKind);
        }
        var repeated = await deployment.Command("quiesce");
        Assert.Equal(0, repeated.Code);
        using var after = JsonDocument.Parse(repeated.Output);
        Assert.Equal("settled", after.RootElement.GetProperty("action").GetString());
        Assert.Equal(1, after.RootElement.GetProperty("jobs").GetInt64());
    }

    [Fact]
    public async Task QuiesceCannotPauseOrStopAnotherActiveOwnerBeforeAcquiringItsLock()
    {
        using var deployment = installation.Deploy();
        Assert.Equal(0, (await deployment.Command("initialize")).Code);
        using var work = LearningFiles.Open(Path.Combine(deployment.Root, "work"));
        using var owner = work.AcquireLock("owner.lock");
        var result = await deployment.Command("quiesce");
        Assert.Equal(2, result.Code);
        Assert.Contains("learning_owner_already_running", result.Error);
        using var status = JsonDocument.Parse((await deployment.Command("status")).Output);
        Assert.False(status.RootElement.GetProperty("paused").GetBoolean());
        Assert.Equal("not_started", status.RootElement.GetProperty("run_state").GetString());
    }
}

[SupportedOSPlatform("linux")]
public sealed class LearningCommandInstallation : IDisposable
{
    internal static readonly string[] ManagedNames = ["cnet-control.dll", "cnet-control.deps.json", "cnet-control.runtimeconfig.json",
        "Microsoft.Data.Sqlite.dll", "SQLitePCLRaw.core.dll", "SQLitePCLRaw.batteries_v2.dll", "SQLitePCLRaw.provider.e_sqlite3.dll",
        "runtimes/linux-x64/native/libe_sqlite3.so"];
    internal static readonly string[] NativeNames = ["cnet_table_capsule", "cnet_table_verify", "cnet_learning_snapshot", "cnet_capsulectl", "cnetd", "libcnet_capsule_core.so"];
    internal const UnixFileMode Private = UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute;
    private readonly string repository = LearningTestRepository.RequireBuilt(NativeNames);
    private readonly string publishRoot = Directory.CreateTempSubdirectory("cnet-command-publish-").FullName;
    internal static string Dotnet => Path.GetFullPath(Path.Combine(RuntimeEnvironment.GetRuntimeDirectory(), "../../..", "dotnet"));
    public LearningCommandInstallation()
    {
        File.SetUnixFileMode(publishRoot, Private);
        var project = Path.Combine(repository, "dotnet/CnetControlPlane/CnetControlPlane.csproj");
        var result = Execute(Dotnet, ["publish", project, "--self-contained", "false", "-p:UseAppHost=false", "-p:DebugType=None",
            "-p:DebugSymbols=false", "-p:RestoreLockedMode=true", "--artifacts-path", Path.Combine(publishRoot, "artifacts"),
            "--output", Path.Combine(publishRoot, "output"), "--nologo"], repository, false).GetAwaiter().GetResult();
        if (result.Code != 0) throw new InvalidOperationException("learning_test_publish_failed: " + result.Error + result.Output);
    }
    public LearningCommandDeployment Deploy() => new(publishRoot, repository);
    internal static async Task<(int Code, string Output, string Error)> Execute(string executable, string[] args, string cwd, bool clean)
    {
        var start = new ProcessStartInfo(executable) { WorkingDirectory = cwd, UseShellExecute = false,
            RedirectStandardOutput = true, RedirectStandardError = true, RedirectStandardInput = true };
        if (clean) start.Environment.Clear();
        foreach (var arg in args) start.ArgumentList.Add(arg);
        using var process = Process.Start(start)!;
        process.StandardInput.Close();
        var output = process.StandardOutput.ReadToEndAsync(); var error = process.StandardError.ReadToEndAsync();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(120));
        try { await process.WaitForExitAsync(timeout.Token); }
        catch { process.Kill(entireProcessTree: true); await process.WaitForExitAsync(); throw; }
        return (process.ExitCode, await output, await error);
    }
    public void Dispose() => Directory.Delete(publishRoot, true);
}

[SupportedOSPlatform("linux")]
public sealed class LearningCommandDeployment : IDisposable
{
    private Process? daemon;
    public string Root { get; } = Directory.CreateTempSubdirectory("cnet-command-").FullName;
    public string ManagedHash { get; }
    internal LearningCommandDeployment(string published, string repository)
    {
        File.SetUnixFileMode(Root, LearningCommandInstallation.Private);
        var managed = Copy(LearningCommandInstallation.ManagedNames, Path.Combine(published, "output"), "managed", false);
        var native = Copy(LearningCommandInstallation.NativeNames, Path.Combine(repository, "bin"), "native", true);
        foreach (var directory in Directory.EnumerateDirectories(Root, "*", SearchOption.AllDirectories))
            Assert.True(File.GetUnixFileMode(directory) == LearningCommandInstallation.Private,
                "LEARNING_COMMAND_FIXTURE_RED private directory: " + Path.GetRelativePath(Root, directory));
        var bytes = JsonSerializer.SerializeToUtf8Bytes(new { schema_version = 1, target = "linux-x64", files = managed });
        ManagedHash = Hash(bytes); Put("managed.json", bytes);
        Put("runtime.json", JsonSerializer.SerializeToUtf8Bytes(new { schema_version = 1, files = native }));
        Put("policy.json", Encoding.UTF8.GetBytes(LearningPolicyTests.Valid));
    }
    private Dictionary<string, string> Copy(string[] names, string source, string tree, bool executable)
    {
        var hashes = new Dictionary<string, string>();
        Directory.CreateDirectory(Path.Combine(Root, tree), LearningCommandInstallation.Private);
        if (!executable)
            foreach (var relative in new[] { "runtimes", "runtimes/linux-x64", "runtimes/linux-x64/native" })
                Directory.CreateDirectory(Path.Combine(Root, tree, relative), LearningCommandInstallation.Private);
        foreach (var name in names)
        {
            var target = Path.Combine(Root, tree, name);
            Directory.CreateDirectory(Path.GetDirectoryName(target)!, LearningCommandInstallation.Private);
            File.Copy(Path.Combine(source, name), target);
            File.SetUnixFileMode(target, UnixFileMode.UserRead | (executable && !name.EndsWith(".so", StringComparison.Ordinal) ? UnixFileMode.UserExecute : 0));
            hashes.Add(name, Hash(File.ReadAllBytes(target)));
        }
        return hashes;
    }
    internal void Put(string relative, byte[] bytes)
    {
        var path = Path.Combine(Root, relative);
        File.WriteAllBytes(path, bytes); File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
    }
    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    public Task<(int Code, string Output, string Error)> Command(string verb, params string[] args) =>
        LearningCommandInstallation.Execute(LearningCommandInstallation.Dotnet,
            [Path.Combine(Root, "managed/cnet-control.dll"), "learning", verb, Root, .. args], Root, true);
    internal async Task StartDaemon()
    {
        Directory.CreateDirectory(Path.Combine(Root, "packs"), LearningCommandInstallation.Private);
        Put("packs/ROUTES.jsonl", Encoding.ASCII.GetBytes("{\"pattern\":\"fixture\",\"pack\":\"fixture\"}\n"));
        var start = new ProcessStartInfo(Path.Combine(Root, "native/cnetd")) { WorkingDirectory = Root, UseShellExecute = false,
            RedirectStandardError = true, RedirectStandardOutput = true };
        start.Environment.Clear();
        foreach (var (name, value) in new Dictionary<string, string>
        {
            ["CNET_PACKS_ROOT"] = Path.Combine(Root, "packs"), ["CNET_MINIMAL_ROOT"] = Root,
            ["CNET_SOCK"] = Path.Combine(Root, "ipc/ask.sock"), ["CNET_CAPSULE_CONTROL_SOCK"] = Path.Combine(Root, "ipc/control.sock"),
            ["CNET_CAPSULE_SETS_DIR"] = Path.Combine(Root, "work/sets"), ["CNET_CAPSULE_STATE_DIR"] = Path.Combine(Root, "work/state"),
            ["CNET_CAPSULE_DATA_ROOT"] = Path.Combine(Root, "work/data"), ["CNET_SELF_ANSWER"] = "0",
            ["CNET_TEACHER_ON_MISS"] = "0", ["CNET_CORE_AUTO_EVOLVE"] = "0",
        }) start.Environment[name] = value;
        daemon = Process.Start(start)!;
        for (var attempt = 0; attempt < 250; attempt++)
        {
            Assert.False(daemon.HasExited, "LEARNING_COMMAND_RED private daemon exited");
            if (File.Exists(Path.Combine(Root, "ipc/ask.sock")) && File.Exists(Path.Combine(Root, "ipc/control.sock"))) return;
            await Task.Delay(20);
        }
        Assert.Fail("LEARNING_COMMAND_RED daemon not ready");
    }
    public void Dispose()
    {
        if (daemon is not null)
        {
            if (!daemon.HasExited) { daemon.Kill(); daemon.WaitForExit(5000); }
            daemon.Dispose();
        }
        foreach (var directory in Directory.EnumerateDirectories(Root, "*", SearchOption.AllDirectories)) File.SetUnixFileMode(directory, LearningCommandInstallation.Private);
        Directory.Delete(Root, true);
    }
}
