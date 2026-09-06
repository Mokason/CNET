using CNET.Cce.Llm.Tools;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Xunit;

namespace CNET.Cce.Llm.Tests;

[CollectionDefinition("Scriptlet isolation", DisableParallelization = true)]
public sealed class ScriptletIsolationCollection { }

[Collection("Scriptlet isolation")]
public sealed class ScriptletIsolationTests
{
    [DllImport("libc", EntryPoint = "open", SetLastError = true)]
    private static extern int Open(string path, int flags);
    [DllImport("libc", EntryPoint = "close")]
    private static extern int Close(int descriptor);
    [DllImport("libc", EntryPoint = "mkfifo")]
    private static extern int MakeFifo(string path, uint mode);

    private static string Raw(string source, string input = "", int timeoutMs = 1000)
    {
        // Intentionally bypass ScriptletGuard: successful denial must come
        // from the worker's OS boundary, not a syntax/reference rejection.
        bool accepted = ScriptletProcess.Run(source, input, false, timeoutMs, 8192,
            out string output, out string error, out int pid);
        Reaped(pid);
        Assert.True(accepted, error);
        return output;
    }

    private static void Reaped(int pid)
    {
        Assert.True(pid > 0, "worker was never launched");
        Assert.False(Directory.Exists($"/proc/{pid}"), $"worker {pid} or its threads remain");
    }

    private static void Refused(string source, int timeoutMs = 150)
    {
        var watch = Stopwatch.StartNew();
        Assert.False(ScriptletProcess.Run(source, "loop", false, timeoutMs, 8192,
            out string output, out string error, out int pid));
        Assert.Empty(output);
        Assert.NotEmpty(error);
        Reaped(pid);
        Assert.True(watch.ElapsedMilliseconds < 6000, $"refusal took {watch.ElapsedMilliseconds} ms");
        Assert.Equal(0, ScriptletProcess.ActiveJobs);
    }

    [Fact]
    public async Task SpecialWorkerArtifactRefusesWithoutBlockingInstallation()
    {
        string distribution = Directory.CreateTempSubdirectory("cnet-scriptlet-special-").FullName;
        string fifo = Path.Combine(distribution, "CNET.Scriptlet.Worker.dll");
        Assert.Equal(0, MakeFifo(fifo, 0x180)); // 0600
        Task<Exception> installation = Task.Run(() => Record.Exception(() => ScriptletProcess.InstallVerifiedWorker(distribution)));
        bool bounded = false;
        try
        {
            bounded = await Task.WhenAny(installation, Task.Delay(300)) == installation;
        }
        finally
        {
            // The RED implementation blocks in open(). Safely unblock this
            // trusted test operation; never leave a test thread abandoned.
            if (!installation.IsCompleted)
            {
                int writer = Open(fifo, 1 | 0x800); // O_WRONLY | O_NONBLOCK
                if (writer >= 0) Close(writer);
            }
            await installation.WaitAsync(TimeSpan.FromSeconds(2));
            Directory.Delete(distribution, recursive: true);
        }
        Assert.True(bounded, "SCRIPTLET_INSTALLATION_RED special artifact blocks before worker deadline");
        Assert.IsType<InvalidDataException>(await installation);
    }

    [Fact]
    public void TamperedNativeLauncherCannotPassTrustedArtifactVerification()
    {
        string distribution = Directory.CreateTempSubdirectory("cnet-scriptlet-tampered-").FullName;
        try
        {
            foreach (string file in Directory.GetFiles(Path.Combine(AppContext.BaseDirectory, "ScriptletWorker")))
                File.Copy(file, Path.Combine(distribution, Path.GetFileName(file)));
            string launcher = Path.Combine(distribution, "cnet-scriptlet-launcher");
            byte[] bytes = File.ReadAllBytes(launcher);
            bytes[^1] ^= 1;
            File.WriteAllBytes(launcher, bytes);
            Assert.Throws<InvalidDataException>(() => ScriptletProcess.InstallVerifiedWorker(distribution));
            Assert.Equal(0, ScriptletProcess.ActiveJobs);
        }
        finally { Directory.Delete(distribution, recursive: true); }
    }

    [Fact]
    public void PublicCompilerCannotBypassSourceGuard()
    {
        Assert.False(ScriptletCompiler.TryCompile(
            "return System.IO.File.ReadAllText(input);", out var function, out var error),
            "SCRIPTLET_ISOLATION_RED public compiler bypasses source guard");
        Assert.Null(function);
        Assert.NotEmpty(error);
    }

    [Fact]
    public void ArbitraryDelegateIsRefusedWithoutInvocation()
    {
        bool invoked = false;
        bool accepted = ScriptletSandbox.TryRun(input =>
        {
            invoked = true;
            return input;
        }, "sentinel", out string output);

        Assert.False(invoked,
            "SCRIPTLET_ISOLATION_RED arbitrary delegate ran with host authority");
        Assert.False(accepted);
        Assert.Empty(output);
    }

    [Fact]
    public void AllowedRegexLinqAndMathStillExecuteInsideWorker()
    {
        Assert.True(ScriptletSandbox.TryRunSource("return Regex.Replace(input, \"[0-9]\", \"#\");", "a1b2", out string replaced));
        Assert.Equal("a#b#", replaced);
        Assert.True(ScriptletSandbox.TryRunSource("return new Regex(\"^[a-z]+$\", RegexOptions.Compiled).IsMatch(input).ToString();", "hello", out string match));
        Assert.Equal("True", match);
        Assert.True(ScriptletSandbox.TryRunSource("return Math.Sqrt(double.Parse(input)).ToString();", "16", out string root));
        Assert.Equal("4", root);
        Assert.True(ScriptletSandbox.TryRunSource("return new string(input.Reverse().ToArray());", "hello", out string reversed));
        Assert.Equal("olleh", reversed);
    }

    [Fact]
    public void SourceClosureCanBeInvokedDirectlyButWrapperDelegateIsRefused()
    {
        Assert.True(ScriptletCompiler.TryCompile("return input.ToUpperInvariant();", out var bound, out var error), error);
        Assert.Equal("HELLO", bound!("hello"));
        bool invoked = false;
        Assert.False(ScriptletSandbox.TryRun(input => { invoked = true; return bound(input); }, "x", out _));
        Assert.False(invoked);
    }

    [Fact]
    public void GuardBypassCannotReadOrWriteHostFiles()
    {
        string directory = Directory.CreateTempSubdirectory("cnet-scriptlet-host-sentinel-").FullName;
        string original = Path.Combine(directory, "private.txt");
        string created = Path.Combine(directory, "created.txt");
        File.WriteAllText(original, "host-only-secret");
        try
        {
            Assert.Equal("denied", Raw("try { return System.IO.File.ReadAllText(input); } catch (UnauthorizedAccessException) { return \"denied\"; }", original));
            Assert.Equal("denied", Raw("try { System.IO.File.WriteAllText(input, \"changed\"); return \"escaped\"; } catch (UnauthorizedAccessException) { return \"denied\"; }", original));
            Assert.Equal("denied", Raw("try { System.IO.File.WriteAllText(input, \"new\"); return \"escaped\"; } catch (UnauthorizedAccessException) { return \"denied\"; }", created));
            Assert.Equal("host-only-secret", File.ReadAllText(original));
            Assert.False(File.Exists(created));
        }
        finally { Directory.Delete(directory, recursive: true); }
    }

    [Fact]
    public void ProcGrantIsPinnedToWorkerMapsOnly()
    {
        Assert.Equal("own:denied", Raw("string own = System.IO.File.ReadAllText(\"/proc/self/maps\").Length > 0 ? \"own:\" : \"empty:\"; try { return own + System.IO.File.ReadAllText(input); } catch (UnauthorizedAccessException) { return own + \"denied\"; }", $"/proc/{Environment.ProcessId}/maps"));
        Assert.Equal("denied", Raw("try { return System.IO.File.ReadAllText(\"/proc/self/environ\"); } catch (UnauthorizedAccessException) { return \"denied\"; }"));
    }

    [Fact]
    public void EnvironmentCredentialsAreNotInherited()
    {
        const string name = "CNET_SCRIPTLET_TEST_SECRET";
        string? previous = Environment.GetEnvironmentVariable(name);
        Environment.SetEnvironmentVariable(name, "host-only-credential");
        try
        {
            Assert.Equal("absent:0", Raw("return (Environment.GetEnvironmentVariable(input) ?? \"absent\") + \":\" + Environment.GetEnvironmentVariable(\"DOTNET_EnableDiagnostics\");", name));
        }
        finally { Environment.SetEnvironmentVariable(name, previous); }
    }

    [Fact]
    public void InheritableHostFileDescriptorIsClosedBeforeRuntimeStartup()
    {
        string file = Path.GetTempFileName();
        File.WriteAllText(file, "descriptor-secret-91ec");
        int descriptor = Open(file, 0); // Deliberately omit O_CLOEXEC.
        Assert.True(descriptor >= 0);
        try
        {
            Assert.Equal("absent", Raw("[System.Runtime.InteropServices.DllImport(\"libc\", EntryPoint=\"pread\")] static extern long Read(int fd, byte[] bytes, ulong count, long offset); byte[] b = new byte[22]; for (int fd=3; fd<256; fd++) { if (Read(fd,b,(ulong)b.Length,0)>0 && Encoding.UTF8.GetString(b).Contains(\"descriptor-secret\")) return \"leaked\"; } return \"absent\";"));
        }
        finally { Close(descriptor); File.Delete(file); }
    }

    [Fact]
    public void NativeBypassCannotCreateProcessesThreadsNetworkOrExternalAuthority()
    {
        string native = "[System.Runtime.InteropServices.DllImport(\"libc\", EntryPoint=\"syscall\", SetLastError=true)] static extern long Call(long n,long a,long b,long c,long d,long e,long f); ";
        // Null/invalid pointers and signal zero keep a policy regression safe:
        // fork/clone would only create another already-Landlocked worker.
        var calls = new (long N, long A, long B, long C)[] {
            (41, 2, 1, 0), (41, 1, 1, 0), (57, 0, 0, 0), (58, 0, 0, 0),
            (56, 0x10000, 0, 0), (435, 0, 0, 0), (59, 0, 0, 0), (322, 0, 0, 0),
            (16, 1, 0x5412, 0), (101, 999, Environment.ProcessId, 0),
            (310, Environment.ProcessId, 0, 0), (434, Environment.ProcessId, 0, 0),
            (62, Environment.ProcessId, 0, 0), (72, 1, 8, Environment.ProcessId),
            (319, 0, 0, 0), (160, 9, 0, 0), (302, 0, 9, 1), (317, 0, 0, 0)
        };
        string source = native + "var b = new StringBuilder(); " + string.Join(" ", calls.Select(c =>
            $"b.Append(Call({c.N},{c.A},{c.B},{c.C},0,0,0)).Append(':').Append(System.Runtime.InteropServices.Marshal.GetLastPInvokeError()).Append(',');")) + " return b.ToString();";
        Assert.Equal(string.Concat(Enumerable.Repeat("-1:1,", calls.Length)), Raw(source));
    }

    [Fact]
    public void PrivilegedIdentityPolicyRefusesRootSetIdAndEveryCapabilityWord()
    {
        // Synthetic predicate inputs: never acquire privileges or invoke
        // destructive privileged advice to prove the startup refusal policy.
        string source = "[System.Runtime.InteropServices.DllImport(\"libcnet-scriptlet-seal.so\", EntryPoint=\"cnet_scriptlet_identity_allowed\")] static extern int Allowed(uint u,uint e,uint g,uint eg,ulong p,ulong c,ulong i); " +
            "return Allowed(1000,1000,1000,1000,0,0,0) + \":\" + Allowed(0,0,1000,1000,0,0,0) + Allowed(1000,1001,1000,1000,0,0,0) + Allowed(1000,1000,0,0,0,0,0) + Allowed(1000,1000,1000,1001,0,0,0) + Allowed(1000,1000,1000,1000,1,0,0) + Allowed(1000,1000,1000,1000,0,4294967296,0) + Allowed(1000,1000,1000,1000,0,0,9223372036854775808);";
        Assert.Equal("1:0000000", Raw(source));
    }

    [Fact]
    public void PathMetadataAndOPathBypassesAreDeniedAfterSeal()
    {
        string source = "[System.Runtime.InteropServices.DllImport(\"libc\", EntryPoint=\"syscall\", SetLastError=true)] static extern long Call(long n,long a,long b,long c,long d,long e,long f); " +
            "[System.Runtime.InteropServices.DllImport(\"libc\", EntryPoint=\"open\", SetLastError=true)] static extern int Open(string path,int flags); " +
            "var output = new StringBuilder(); foreach(int n in new int[]{4,6,21,89,217,262,267,269,332,439}) { output.Append(Call(n,0,0,0,0,0,0)).Append(':').Append(System.Runtime.InteropServices.Marshal.GetLastPInvokeError()).Append(','); } " +
            "output.Append(Open(\"/etc/passwd\",0x200000)).Append(':').Append(System.Runtime.InteropServices.Marshal.GetLastPInvokeError()).Append(','); return output.ToString();";
        Assert.Equal(string.Concat(Enumerable.Repeat("-1:1,", 11)), Raw(source));
    }

    [Fact]
    public void GuardBypassCannotLaunchExecutable()
    {
        Assert.Equal("denied", Raw("try { System.Diagnostics.Process.Start(\"/usr/bin/true\"); return \"escaped\"; } catch { return \"denied\"; }"));
    }

    [Fact]
    public void GuardBypassCannotCreateManagedThread()
    {
        Assert.Equal("denied", Raw("try { var thread = new System.Threading.Thread(() => {}); thread.Start(); thread.Join(); return \"escaped\"; } catch { return \"denied\"; }"));
    }

    [Fact]
    public async Task DirectWorkerLaunchRefusesBeforeConsumingSource()
    {
        string file = Path.GetTempFileName();
        File.WriteAllText(file, "original");
        string runtime = RuntimeEnvironment.GetRuntimeDirectory();
        string dotnet = Path.Combine(Directory.GetParent(runtime.TrimEnd('/'))!.Parent!.Parent!.FullName, "dotnet");
        using var process = new Process { StartInfo = new ProcessStartInfo(dotnet) {
            UseShellExecute = false, RedirectStandardInput = true,
            RedirectStandardOutput = true, RedirectStandardError = true } };
        process.StartInfo.ArgumentList.Add(Path.Combine(AppContext.BaseDirectory, "ScriptletWorker", "CNET.Scriptlet.Worker.dll"));
        try
        {
            Assert.True(process.Start());
            byte[] request = JsonSerializer.SerializeToUtf8Bytes(new {
                Source = "System.IO.File.WriteAllText(input, \"escaped\"); return input;", Input = file, CompileOnly = false });
            byte[] header = BitConverter.GetBytes(request.Length);
            await process.StandardInput.BaseStream.WriteAsync(header);
            await process.StandardInput.BaseStream.WriteAsync(request);
            process.StandardInput.Close();
            using var deadline = new CancellationTokenSource(5000);
            await process.WaitForExitAsync(deadline.Token);
            Assert.Equal(121, process.ExitCode);
            Assert.Empty(await process.StandardOutput.ReadToEndAsync(deadline.Token));
            Assert.Empty(await process.StandardError.ReadToEndAsync(deadline.Token));
            Assert.Equal("original", File.ReadAllText(file));
        }
        finally
        {
            if (!process.HasExited) { process.Kill(entireProcessTree: true); process.WaitForExit(); }
            File.Delete(file);
        }
    }

    [Fact]
    public void AddressSpaceAndManagedHeapAreBounded()
    {
        Assert.Equal("-1:12", Raw("[System.Runtime.InteropServices.DllImport(\"libc\", EntryPoint=\"mmap\", SetLastError=true)] static extern long Map(long addr, ulong length, int prot, int flags, int fd, long offset); long result=Map(0,3221225472,0,0x22,-1,0); return result + \":\" + System.Runtime.InteropServices.Marshal.GetLastPInvokeError();"));
        Assert.True(long.Parse(Raw("return GC.GetGCMemoryInfo().TotalAvailableMemoryBytes.ToString();")) <= 134217728);
    }

    [Theory]
    [InlineData("int n=0; for (;input.Length>0;) n++; return n.ToString();")]
    [InlineData("for (;input.Length>0;) Console.Write(new string('x',1024)); return input;")]
    [InlineData("for (;input.Length>0;) Console.Error.Write(new string('x',1024)); return input;")]
    [InlineData("return new string('x', 8193);")]
    [InlineData("Console.Write(\"forged result\"); return input;")]
    [InlineData("Environment.Exit(7); return input;")]
    [InlineData("var allocations = new List<byte[]>(); for (;input.Length>0;) allocations.Add(new byte[1048576]); return input;")]
    public void HostileExecutionIsBoundedAndWorkerIsReaped(string source) => Refused(source);

    [Fact]
    public void ExecutionDeadlineAndReapAreTightAfterReady()
    {
        var measurement = new ScriptletProcess.Measurement();
        Assert.False(ScriptletProcess.Run("long n=0; for (;input.Length>0;) n++; return n.ToString();",
            "loop", false, 150, 8192, out _, out string error, out int pid, measurement));
        Assert.True(measurement.Ready);
        Assert.Equal("scriptlet_timeout", error);
        Assert.InRange(measurement.ExecutionAndReapMilliseconds, 100, 1000);
        Reaped(pid);
    }

    [Fact]
    public async Task CapacityRefusalDoesNotStartAnotherWorker()
    {
        const string source = "long n=0; for (;input.Length>0;) n++; return n.ToString();";
        static (bool Accepted, int Pid) Invoke() { bool accepted = ScriptletProcess.Run(source, "loop", false, 1000, 8192, out _, out _, out int id); return (accepted, id); }
        Task<(bool Accepted, int Pid)> first = Task.Run(Invoke);
        Task<(bool Accepted, int Pid)> second = Task.Run(Invoke);
        Assert.True(SpinWait.SpinUntil(() => ScriptletProcess.ActiveJobs == 2, 2000));
        Assert.False(ScriptletProcess.Run("return input;", "x", false, 150, 8192, out _, out string error, out int pid));
        Assert.Equal("scriptlet_capacity_exhausted", error);
        Assert.Equal(0, pid);
        var results = await Task.WhenAll(first, second);
        foreach (var result in results) { Assert.False(result.Accepted); Reaped(result.Pid); }
        Assert.Equal(0, ScriptletProcess.ActiveJobs);
    }
}
