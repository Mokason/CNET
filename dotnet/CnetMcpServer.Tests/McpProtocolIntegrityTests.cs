using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Xunit;

namespace CnetMcpServer.Tests;

public sealed class McpProtocolIntegrityTests
{
    private sealed record RunResult(string Stdout, string Stderr, int ExitCode);

    private static string FindServerPath()
    {
        string baseDir = AppContext.BaseDirectory;
        foreach (string name in new[] { "CnetMcpServer", "CnetMcpServer.dll" })
        {
            string direct = Path.Combine(baseDir, name);
            if (File.Exists(direct)) return direct;
        }
        for (int depth = 0; depth < 6; depth++)
        {
            baseDir = Path.GetDirectoryName(baseDir)
                ?? throw new FileNotFoundException("CnetMcpServer executable not found");
            foreach (string name in new[] { "CnetMcpServer", "CnetMcpServer.dll" })
            {
                string candidate = Path.Combine(baseDir, "CnetMcpServer", name);
                if (File.Exists(candidate)) return candidate;
            }
        }
        throw new FileNotFoundException("CnetMcpServer executable not found");
    }

    private static string FindRepoFile(string name)
    {
        string? directory = AppContext.BaseDirectory;
        for (int depth = 0; depth < 10 && directory is not null; depth++)
        {
            string candidate = Path.Combine(directory, name);
            if (File.Exists(candidate)) return candidate;
            directory = Path.GetDirectoryName(directory);
        }
        throw new FileNotFoundException($"Repository fixture not found: {name}");
    }

    private static Process StartServer(int compressionDelayMs = 0, string? basePath = null)
    {
        string serverPath = FindServerPath();
        bool isDll = serverPath.EndsWith(".dll", StringComparison.Ordinal);
        var startInfo = new ProcessStartInfo
        {
            FileName = isDll ? "dotnet" : serverPath,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        if (isDll) startInfo.ArgumentList.Add(serverPath);
        startInfo.Environment["CNET_BASE_PATH"] = basePath ??
            "/tmp/cnet_mcp_protocol_integrity_missing.cnb";
        startInfo.Environment["CNET_HEALTH_TICK_SECONDS"] = "0";
        startInfo.Environment["CNET_MCP_TEST_COMPRESSION_DELAY_MS"] =
            compressionDelayMs.ToString(System.Globalization.CultureInfo.InvariantCulture);
        var process = new Process { StartInfo = startInfo };
        process.Start();
        return process;
    }

    private static async Task<RunResult> RunServerAsync(params string[] frames)
    {
        using Process process = StartServer();
        Task<string> stdoutTask = process.StandardOutput.ReadToEndAsync();
        Task<string> stderrTask = process.StandardError.ReadToEndAsync();
        foreach (string frame in frames)
            await process.StandardInput.WriteLineAsync(frame);
        process.StandardInput.Close();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        await process.WaitForExitAsync(timeout.Token);
        return new RunResult(await stdoutTask, await stderrTask, process.ExitCode);
    }

    private static List<JsonElement> ParseFrames(string stdout)
    {
        var frames = new List<JsonElement>();
        foreach (string line in stdout.Split('\n', StringSplitOptions.RemoveEmptyEntries))
            frames.Add(JsonSerializer.Deserialize<JsonElement>(line.Trim()));
        return frames;
    }

    private static JsonElement FindById(IEnumerable<JsonElement> frames, int id)
    {
        return frames.Single(frame =>
            frame.ValueKind == JsonValueKind.Object &&
            frame.TryGetProperty("id", out JsonElement value) &&
            value.ValueKind == JsonValueKind.Number && value.GetInt32() == id);
    }

    private static void AssertError(JsonElement frame, int code)
    {
        Assert.Equal("2.0", frame.GetProperty("jsonrpc").GetString());
        Assert.Equal(code, frame.GetProperty("error").GetProperty("code").GetInt32());
    }

    [Fact]
    public async Task Invalid_Envelopes_And_Params_Do_Not_Kill_Stream_And_Notifications_Are_Silent()
    {
        RunResult run = await RunServerAsync(
            "42",
            "[]",
            "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"tools/list\"}",
            "{\"id\":2,\"method\":\"tools/list\"}",
            "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"initialize\",\"params\":null}",
            "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":7}",
            "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\",\"params\":{}}",
            "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\"}",
            "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"params\":null}",
            "{\"jsonrpc\":\"2.0\",\"method\":\"unknown/notification\"}",
            "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\"}",
            "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"tools/list\"}");

        Assert.Equal(0, run.ExitCode);
        List<JsonElement> responses = ParseFrames(run.Stdout);
        Assert.Equal(7, responses.Count);
        Assert.Equal(2, responses.Count(frame =>
            frame.TryGetProperty("id", out JsonElement id) &&
            id.ValueKind == JsonValueKind.Null &&
            frame.GetProperty("error").GetProperty("code").GetInt32() == -32600));
        AssertError(FindById(responses, 1), -32600);
        AssertError(FindById(responses, 2), -32600);
        AssertError(FindById(responses, 3), -32602);
        AssertError(FindById(responses, 4), -32602);
        Assert.True(FindById(responses, 99).TryGetProperty("result", out _));
        Assert.All(responses, frame => Assert.Equal(JsonValueKind.Object, frame.ValueKind));
        Console.WriteLine("MCP_PROTOCOL_SURVIVAL_PASS");
    }

    [Fact]
    public async Task Same_Process_Reloads_Atomically_Replaced_Base_Generation()
    {
        string tempDirectory = Path.Combine(Path.GetTempPath(), $"cnet-mcp-reload-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDirectory);
        string liveBase = Path.Combine(tempDirectory, "live.cnb");
        string stagedBase = Path.Combine(tempDirectory, "live.cnb.next");
        File.Copy(FindRepoFile("tmp_soul_host.cnb"), liveBase);

        try
        {
            using Process process = StartServer(basePath: liveBase);
            Task<string> stderrTask = process.StandardError.ReadToEndAsync();

            async Task<string> CallListUnits(int id)
            {
                await process.StandardInput.WriteLineAsync(
                    $"{{\"jsonrpc\":\"2.0\",\"id\":{id},\"method\":\"tools/call\",\"params\":{{\"name\":\"cnet_list_units\",\"arguments\":{{}}}}}}");
                await process.StandardInput.FlushAsync();
                using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
                string? line = await process.StandardOutput.ReadLineAsync(timeout.Token);
                Assert.NotNull(line);
                JsonElement frame = JsonSerializer.Deserialize<JsonElement>(line);
                return frame.GetProperty("result").GetProperty("content")[0]
                    .GetProperty("text").GetString() ?? "";
            }

            string before = await CallListUnits(1);
            Assert.Contains("[CNET] 1 certified units", before, StringComparison.Ordinal);

            File.Copy(FindRepoFile("flagship.cnb"), stagedBase);
            File.Move(stagedBase, liveBase, overwrite: true);
            File.SetLastWriteTimeUtc(liveBase, DateTime.UtcNow.AddSeconds(2));

            string after = await CallListUnits(2);
            Assert.Contains("[CNET] 256 certified units", after, StringComparison.Ordinal);

            process.StandardInput.Close();
            using var exitTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            await process.WaitForExitAsync(exitTimeout.Token);
            await stderrTask;
            Assert.Equal(0, process.ExitCode);
            Console.WriteLine("MCP_BASE_GENERATION_RELOAD_PASS");
        }
        finally
        {
            Directory.Delete(tempDirectory, recursive: true);
        }
    }

    [Fact]
    public async Task Blocking_Independent_Compression_Does_Not_Block_ToolsList()
    {
        using Process process = StartServer(compressionDelayMs: 2000);
        Task<string> stderrTask = process.StandardError.ReadToEndAsync();
        var stopwatch = Stopwatch.StartNew();
        await process.StandardInput.WriteLineAsync(
            "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"params\":{\"name\":\"cnet_compress_model\",\"arguments\":{\"model_path\":\"\"}}}");
        await process.StandardInput.WriteLineAsync(
            "{\"jsonrpc\":\"2.0\",\"id\":77,\"method\":\"tools/list\"}");
        await process.StandardInput.FlushAsync();

        using var responseTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(1));
        string? response = await process.StandardOutput.ReadLineAsync(responseTimeout.Token);
        Assert.NotNull(response);
        JsonElement frame = JsonSerializer.Deserialize<JsonElement>(response);
        Assert.Equal(77, frame.GetProperty("id").GetInt32());
        Assert.True(frame.TryGetProperty("result", out _));
        Assert.True(stopwatch.Elapsed < TimeSpan.FromSeconds(1),
            $"tools/list was blocked for {stopwatch.Elapsed}");

        process.StandardInput.Close();
        using var exitTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(6));
        await process.WaitForExitAsync(exitTimeout.Token);
        await stderrTask;
        Assert.Equal(0, process.ExitCode);
        Assert.True(stopwatch.Elapsed >= TimeSpan.FromMilliseconds(1500),
            "injected compression delay was not active, so concurrency was not exercised");
        Assert.True(await process.StandardOutput.ReadLineAsync() is null,
            "compression notification must remain silent");
        Console.WriteLine("MCP_LOCK_SCOPE_CONCURRENCY_PASS");
    }
}
