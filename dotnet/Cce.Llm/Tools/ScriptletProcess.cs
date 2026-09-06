using System.Buffers.Binary;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;

namespace CNET.Cce.Llm.Tools;

/// <summary>Trusted host boundary for the Linux scriptlet worker.</summary>
internal static class ScriptletProcess
{
    [StructLayout(LayoutKind.Explicit, Size = 144)] // Linux x86-64 struct stat.
    private struct FileStatus { [FieldOffset(24)] internal uint Mode; }
    [DllImport("libc", EntryPoint = "open", SetLastError = true)]
    private static extern int OpenArtifact(string path, int flags);
    [DllImport("libc", EntryPoint = "fstat", SetLastError = true)]
    private static extern int FileStat(SafeFileHandle handle, out FileStatus status);
    private const int StartupTimeoutMs = 5000;
    private static readonly SemaphoreSlim Jobs = new(2, 2);
    private static readonly Lazy<string> Installation = new(() =>
        InstallVerifiedWorker(Path.Combine(AppContext.BaseDirectory, "ScriptletWorker")));
    internal static int ActiveJobs => 2 - Jobs.CurrentCount;
    internal sealed class Measurement
    {
        internal bool Ready { get; set; }
        internal long ExecutionAndReapMilliseconds { get; set; }
    }
    private sealed record Request(string Source, string Input, bool CompileOnly);

    internal static bool Run(string source, string input, bool compileOnly,
        int timeoutMs, int maxOutput, out string output, out string error, out int pid,
        Measurement? measurement = null)
    {
        output = ""; error = "invalid_scriptlet_request"; pid = 0;
        if (source is null || source.Length > 4000 || input is null || input.Length > 8192 ||
            timeoutMs is < 1 or > 10000 || maxOutput is < 0 or > 8192) return false;
        if (!OperatingSystem.IsLinux() || RuntimeInformation.ProcessArchitecture != Architecture.X64)
        { error = "scriptlet_isolation_unavailable"; return false; }
        if (!Jobs.Wait(0)) { error = "scriptlet_capacity_exhausted"; return false; }
        try
        {
            var result = RunAsync(source, input, compileOnly, timeoutMs, maxOutput, measurement)
                .GetAwaiter().GetResult();
            output = result.Output; error = result.Error; pid = result.Pid;
            return result.Success;
        }
        finally { Jobs.Release(); }
    }

    private sealed record Result(bool Success, string Output, string Error, int Pid);

    private static async Task<Result> RunAsync(string source, string input, bool compileOnly,
        int timeoutMs, int maxOutput, Measurement? measurement)
    {
        using var process = new Process();
        using var lifetime = new CancellationTokenSource(StartupTimeoutMs + timeoutMs);
        Task<byte[]>? stderr = null;
        Task<string>? protocol = null;
        var executionWatch = new Stopwatch();
        int pid = 0;
        try
        {
            string installation = Installation.Value;
            string runtime = RuntimeEnvironment.GetRuntimeDirectory().TrimEnd(Path.DirectorySeparatorChar);
            string? dotnetRoot = Directory.GetParent(runtime)?.Parent?.Parent?.FullName;
            if (dotnetRoot is null) return new(false, "", "scriptlet_runtime_unavailable", 0);
            var start = new ProcessStartInfo(Path.Combine(installation, "cnet-scriptlet-launcher"))
            {
                UseShellExecute = false, RedirectStandardInput = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                CreateNoWindow = true, WorkingDirectory = installation
            };
            start.Environment.Clear();
            start.ArgumentList.Add(Path.Combine(dotnetRoot, "dotnet"));
            start.ArgumentList.Add(installation); start.ArgumentList.Add(runtime);
            process.StartInfo = start;
            if (!process.Start()) return new(false, "", "scriptlet_launch_refused", 0);
            pid = process.Id;
            stderr = ReadBoundedAsync(process.StandardError.BaseStream, 1024, lifetime.Token);
            protocol = ExchangeAsync(process, source, input, compileOnly,
                                                  timeoutMs, maxOutput, lifetime.Token, executionWatch, measurement);
            Task first = await Task.WhenAny(protocol, stderr).ConfigureAwait(false);
            if (first == stderr) _ = await stderr.ConfigureAwait(false);
            string output = await protocol.ConfigureAwait(false);
            if ((await stderr.ConfigureAwait(false)).Length != 0 || process.ExitCode != 0)
                return new(false, "", "scriptlet_worker_refused", pid);
            return new(true, output, "", pid);
        }
        catch (OperationCanceledException) { return new(false, "", "scriptlet_timeout", pid); }
        catch (Exception ex) when (ex is IOException or InvalidOperationException or
            InvalidDataException or
            System.ComponentModel.Win32Exception or UnauthorizedAccessException or
            CryptographicException or DecoderFallbackException)
        { return new(false, "", "scriptlet_isolation_unavailable_or_refused", pid); }
        finally
        {
            lifetime.Cancel();
            if (pid != 0)
            {
                if (!process.HasExited) process.Kill(entireProcessTree: true);
                // A refusal is not complete until the worker has actually exited.
                if (!process.WaitForExit(3000))
                    throw new InvalidOperationException("scriptlet_worker_reap_failed");
            }
            if (stderr is not null)
                try { await stderr.ConfigureAwait(false); } catch (Exception) { }
            if (protocol is not null)
                try { await protocol.ConfigureAwait(false); } catch (Exception) { }
            executionWatch.Stop();
            if (measurement is not null)
                measurement.ExecutionAndReapMilliseconds = executionWatch.ElapsedMilliseconds;
        }
    }

    private static async Task<string> ExchangeAsync(Process process, string source, string input,
        bool compileOnly, int timeoutMs, int maxOutput, CancellationToken lifetime,
        Stopwatch executionWatch, Measurement? measurement)
    {
        using var startup = CancellationTokenSource.CreateLinkedTokenSource(lifetime);
        startup.CancelAfter(StartupTimeoutMs);
        byte[] request = JsonSerializer.SerializeToUtf8Bytes(new Request(source, input, compileOnly));
        if (request.Length > 65536) throw new InvalidDataException("request bounds");
        byte[] header = new byte[4]; BinaryPrimitives.WriteInt32LittleEndian(header, request.Length);
        Stream stdin = process.StandardInput.BaseStream;
        await stdin.WriteAsync(header, startup.Token).ConfigureAwait(false);
        await stdin.WriteAsync(request, startup.Token).ConfigureAwait(false);
        process.StandardInput.Close();
        Stream stdout = process.StandardOutput.BaseStream;
        byte[] expected = compileOnly ? "COMPILED\n"u8.ToArray() : "READY\n"u8.ToArray();
        byte[] received = new byte[expected.Length];
        await stdout.ReadExactlyAsync(received, startup.Token).ConfigureAwait(false);
        if (!received.AsSpan().SequenceEqual(expected)) throw new InvalidDataException("worker framing");
        executionWatch.Start();
        if (measurement is not null) measurement.Ready = true;
        using var execution = CancellationTokenSource.CreateLinkedTokenSource(lifetime);
        execution.CancelAfter(timeoutMs);
        string result = "";
        if (!compileOnly)
        {
            await stdout.ReadExactlyAsync(header, execution.Token).ConfigureAwait(false);
            int length = BinaryPrimitives.ReadInt32LittleEndian(header);
            if (length < 0 || length > maxOutput * 4) throw new InvalidDataException("output bounds");
            byte[] body = new byte[length];
            await stdout.ReadExactlyAsync(body, execution.Token).ConfigureAwait(false);
            result = new UTF8Encoding(false, true).GetString(body);
            if (result.Length > maxOutput) throw new InvalidDataException("output bounds");
        }
        byte[] extra = new byte[1];
        if (await stdout.ReadAsync(extra, execution.Token).ConfigureAwait(false) != 0)
            throw new InvalidDataException("trailing worker output");
        await process.WaitForExitAsync(execution.Token).ConfigureAwait(false);
        return result;
    }

    private static async Task<byte[]> ReadBoundedAsync(Stream stream, int cap, CancellationToken token)
    {
        byte[] buffer = new byte[cap + 1]; int count = 0;
        while (count <= cap)
        {
            int read = await stream.ReadAsync(buffer.AsMemory(count), token).ConfigureAwait(false);
            if (read == 0) return buffer[..count];
            count += read;
        }
        throw new InvalidDataException("worker diagnostic output bounds");
    }

    // Internal for integrity tests. Production always uses the fixed packaged
    // directory above; neither request JSON nor public APIs select artifacts.
    internal static string InstallVerifiedWorker(string distribution)
    {
        if (!OperatingSystem.IsLinux() || RuntimeInformation.ProcessArchitecture != Architecture.X64)
            throw new InvalidOperationException("unsupported platform");
        using Stream manifest = typeof(ScriptletProcess).Assembly.GetManifestResourceStream("ScriptletWorker.sha256")
            ?? throw new InvalidOperationException("worker manifest absent");
        using var reader = new StreamReader(manifest);
        string[] names = ["CNET.Scriptlet.Worker.dll", "CNET.Scriptlet.Worker.deps.json",
            "CNET.Scriptlet.Worker.runtimeconfig.json", "Microsoft.CodeAnalysis.dll",
            "Microsoft.CodeAnalysis.CSharp.dll", "cnet-scriptlet-launcher", "libcnet-scriptlet-seal.so"];
        var hashes = new Dictionary<string, string>(StringComparer.Ordinal);
        string? line;
        while ((line = reader.ReadLine()) is not null)
        {
            string[] fields = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (fields.Length != 2 || !names.Contains(fields[0], StringComparer.Ordinal) ||
                fields[1].Length != 64 || !hashes.TryAdd(fields[0], fields[1]))
                throw new InvalidDataException("invalid worker manifest");
        }
        if (hashes.Count != names.Length) throw new InvalidDataException("incomplete worker manifest");
        string directory = Directory.CreateTempSubdirectory("cnet-scriptlet-worker-").FullName;
        File.SetUnixFileMode(directory, UnixFileMode.UserRead|UnixFileMode.UserWrite|UnixFileMode.UserExecute);
        try
        {
            foreach (string name in names)
            {
                string path = Path.Combine(distribution, name);
                // Nonblocking, nofollow open before inspecting type: a FIFO or
                // symlink must never block installation outside the worker.
                int descriptor = OpenArtifact(path, 0x800 | 0x20000 | 0x80000);
                if (descriptor < 0) throw new InvalidDataException("worker artifact open refused");
                using var handle = new SafeFileHandle((IntPtr)descriptor, ownsHandle: true);
                if (FileStat(handle, out FileStatus status) != 0 || (status.Mode & 0xf000) != 0x8000)
                    throw new InvalidDataException("worker artifact is not regular");
                using var artifact = new FileStream(handle, FileAccess.Read);
                if (!artifact.CanSeek || artifact.Length is < 1 or > 32 * 1024 * 1024)
                    throw new InvalidDataException("worker artifact bounds");
                byte[] bytes = new byte[(int)artifact.Length];
                artifact.ReadExactly(bytes);
                if (artifact.ReadByte() != -1) throw new InvalidDataException("worker artifact changed");
                if (!string.Equals(Convert.ToHexString(SHA256.HashData(bytes)), hashes[name], StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("worker artifact digest mismatch");
                string destination = Path.Combine(directory, name);
                File.WriteAllBytes(destination, bytes);
                File.SetUnixFileMode(destination, name == "cnet-scriptlet-launcher"
                    ? UnixFileMode.UserRead|UnixFileMode.UserExecute : UnixFileMode.UserRead);
            }
            AppDomain.CurrentDomain.ProcessExit += (_, _) =>
            {
                try { Directory.Delete(directory, recursive: true); } catch (IOException) { }
            };
            return directory;
        }
        catch { Directory.Delete(directory, recursive: true); throw; }
    }
}
