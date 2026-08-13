using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace CnetControlPlane.Hermes;

public static class HermesWrapperRunner
{
    public static string Sha256File(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }

    public static string ValidateManifest(JsonElement manifest)
    {
        if (!manifest.TryGetProperty("schema_version", out var sv) || sv.GetInt32() != 1
            || !manifest.TryGetProperty("kind", out var kind) || kind.GetString() != "cnet-hermes-wrapper")
        {
            throw new ArgumentException("unsupported Hermes wrapper manifest");
        }
        var runtime = manifest.GetProperty("runtime");
        if (runtime.GetProperty("backend").GetString() != "llama-server")
            throw new ArgumentException("only llama-server wrappers are supported");
        if (runtime.GetProperty("cpu_only").ValueKind != JsonValueKind.True
            || runtime.GetProperty("gpu_layers").GetInt32() != 0)
        {
            throw new ArgumentException("wrapper must be CPU-only with gpu_layers=0");
        }
        var host = runtime.GetProperty("host").GetString();
        if (host is not ("127.0.0.1" or "localhost"))
            throw new ArgumentException("wrapper must bind to loopback");
        if (runtime.GetProperty("ctx_size").GetInt32() < 65536)
            throw new ArgumentException("Hermes wrappers require ctx_size >= 65536");

        var selected = manifest.GetProperty("selected_model");
        var role = selected.GetProperty("role").GetString();
        if (role is not ("reference" or "candidate"))
            throw new ArgumentException("wrapper lacks a valid selected role");
        var path = Path.GetFullPath(selected.GetProperty("path").GetString()!);
        if (!File.Exists(path))
            throw new ArgumentException($"selected model not found: {path}");
        using (var fs = File.OpenRead(path))
        {
            var magic = new byte[4];
            if (fs.Read(magic, 0, 4) != 4 || Encoding.ASCII.GetString(magic) != "GGUF")
                throw new ArgumentException($"selected model is not GGUF: {path}");
        }
        var expected = selected.GetProperty("sha256").GetString() ?? "";
        if (expected.Length != 64 || Sha256File(path) != expected)
            throw new ArgumentException("selected model SHA-256 mismatch");

        var hermes = manifest.GetProperty("hermes");
        if (hermes.GetProperty("provider").GetString() != "custom"
            || string.IsNullOrEmpty(hermes.GetProperty("model").GetString()))
        {
            throw new ArgumentException("Hermes provider/model is not configured");
        }
        if (string.IsNullOrEmpty(hermes.GetProperty("default_probe").GetString())
            || string.IsNullOrEmpty(hermes.GetProperty("probe_expected").GetString()))
        {
            throw new ArgumentException("Hermes probe contract is incomplete");
        }
        var maxOutput = hermes.GetProperty("max_output_tokens").GetInt32();
        if (maxOutput is < 1 or > 256)
            throw new ArgumentException("Hermes max_output_tokens must be between 1 and 256");
        var queryTimeout = hermes.GetProperty("query_timeout_seconds").GetDouble();
        if (queryTimeout is < 180 or > 600)
            throw new ArgumentException("Hermes query_timeout_seconds must be between 180 and 600");
        var maxTurns = hermes.GetProperty("max_turns").GetInt32();
        if (maxTurns is < 1 or > 3)
            throw new ArgumentException("Hermes max_turns must be between 1 and 3");
        return path;
    }

    public static List<string> BuildServerCommand(JsonElement manifest, string model, int port)
    {
        var runtime = manifest.GetProperty("runtime");
        var hermes = manifest.GetProperty("hermes");
        return
        [
            runtime.GetProperty("server").GetString()!,
            "--model", model,
            "--alias", hermes.GetProperty("model").GetString()!,
            "--host", "127.0.0.1",
            "--port", port.ToString(),
            "--n-gpu-layers", "0",
            "--threads", (runtime.TryGetProperty("threads", out var t) ? t.GetInt32() : 4).ToString(),
            "--ctx-size", (runtime.TryGetProperty("ctx_size", out var c) ? c.GetInt32() : 8192).ToString(),
            "--parallel", (runtime.TryGetProperty("parallel", out var p) ? p.GetInt32() : 1).ToString(),
            "--jinja",
            "--chat-template-kwargs", "{\"enable_thinking\":false}",
            "--reasoning", "off",
            "--no-webui",
        ];
    }

    public static List<string> BuildHermesCommand(JsonElement manifest, string query, string hermesExecutable = "hermes")
    {
        var settings = manifest.GetProperty("hermes");
        return
        [
            hermesExecutable, "chat",
            "--query", query,
            "--model", settings.GetProperty("model").GetString()!,
            "--provider", settings.GetProperty("provider").GetString()!,
            "--toolsets", "",
            "--ignore-rules",
            "--max-turns", settings.GetProperty("max_turns").GetInt32().ToString(),
            "--quiet",
        ];
    }

    public static Dictionary<string, object?> BuildHermesConfig(JsonElement manifest, string baseUrl)
    {
        var settings = manifest.GetProperty("hermes");
        return new Dictionary<string, object?>
        {
            ["model"] = new Dictionary<string, object?>
            {
                ["default"] = settings.GetProperty("model").GetString(),
                ["provider"] = settings.GetProperty("provider").GetString(),
                ["base_url"] = baseUrl,
                ["api_mode"] = settings.TryGetProperty("api_mode", out var am) ? am.GetString() : "chat_completions",
                ["context_length"] = manifest.GetProperty("runtime").GetProperty("ctx_size").GetInt32(),
                ["max_tokens"] = settings.GetProperty("max_output_tokens").GetInt32(),
            },
        };
    }

    public static bool ProbeMatched(string output, string expected)
    {
        var lines = output.Split('\n')
            .Select(l => l.Trim())
            .Where(l => l.Length > 0)
            .ToList();
        if (lines.Count == 0 || lines.Any(l => l.Contains("Reached maximum iterations")))
            return false;
        return lines[^1] == expected;
    }

    private static int FreePort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        return port;
    }

    private static async Task<bool> HealthyAsync(int port)
    {
        try
        {
            using var client = new HttpClient { Timeout = TimeSpan.FromSeconds(2) };
            var json = await client.GetStringAsync($"http://127.0.0.1:{port}/health");
            using var doc = JsonDocument.Parse(json);
            return doc.RootElement.TryGetProperty("status", out var s) && s.GetString() == "ok";
        }
        catch
        {
            return false;
        }
    }

    private static void StopProcess(Process process)
    {
        if (process.HasExited)
            return;
        try
        {
            process.Kill(entireProcessTree: true);
            process.WaitForExit(10_000);
        }
        catch
        {
            /* ignore */
        }
    }

    public static async Task<Dictionary<string, object?>> RunWrapperAsync(
        JsonElement manifest,
        string model,
        string query,
        string hermesExecutable,
        double startupTimeout,
        double queryTimeout)
    {
        var serverPath = Path.GetFullPath(manifest.GetProperty("runtime").GetProperty("server").GetString()!);
        if (!File.Exists(serverPath))
            throw new ArgumentException($"llama-server is not executable: {serverPath}");
        var port = manifest.GetProperty("runtime").TryGetProperty("port", out var p) && p.GetInt32() != 0
            ? p.GetInt32() : FreePort();
        var serverCommand = BuildServerCommand(manifest, model, port);
        var logPath = Path.Combine(Path.GetTempPath(), $"cnet-hermes-server-{Guid.NewGuid():N}.log");
        var baseUrl = $"http://127.0.0.1:{port}/v1";

        var psi = new ProcessStartInfo
        {
            FileName = serverCommand[0],
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        foreach (var arg in serverCommand.Skip(1))
            psi.ArgumentList.Add(arg);
        psi.Environment["CUDA_VISIBLE_DEVICES"] = "";
        psi.Environment["HIP_VISIBLE_DEVICES"] = "";
        psi.Environment["ROCR_VISIBLE_DEVICES"] = "";
        psi.Environment["CUSTOM_BASE_URL"] = baseUrl;
        psi.Environment["HERMES_DASHBOARD_AUTOSTART"] = "0";

        var process = Process.Start(psi) ?? throw new InvalidOperationException("failed to start llama-server");
        var started = DateTime.UtcNow;
        try
        {
            var deadline = started.AddSeconds(startupTimeout);
            while (!await HealthyAsync(port))
            {
                if (process.HasExited)
                {
                    var tail = File.Exists(logPath) ? await File.ReadAllTextAsync(logPath) : "";
                    throw new InvalidOperationException($"llama-server exited during startup\n{tail[^Math.Min(tail.Length, 4000)..]}");
                }
                if (DateTime.UtcNow >= deadline)
                {
                    var tail = File.Exists(logPath) ? await File.ReadAllTextAsync(logPath) : "";
                    throw new TimeoutException($"llama-server startup timed out\n{tail[^Math.Min(tail.Length, 4000)..]}");
                }
                await Task.Delay(500);
            }

            var readySeconds = (DateTime.UtcNow - started).TotalSeconds;
            var command = BuildHermesCommand(manifest, query, hermesExecutable);
            var queryStarted = DateTime.UtcNow;
            var hermesHome = Directory.CreateTempSubdirectory("cnet-hermes-home-");
            try
            {
                var config = BuildHermesConfig(manifest, baseUrl);
                await File.WriteAllTextAsync(
                    Path.Combine(hermesHome.FullName, "config.yaml"),
                    JsonSerializer.Serialize(config, JsonUtil.Indented) + "\n");
                var hermesPsi = new ProcessStartInfo
                {
                    FileName = command[0],
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    UseShellExecute = false,
                };
                foreach (var arg in command.Skip(1))
                    hermesPsi.ArgumentList.Add(arg);
                hermesPsi.Environment["CUSTOM_BASE_URL"] = baseUrl;
                hermesPsi.Environment["HERMES_HOME"] = hermesHome.FullName;
                hermesPsi.Environment["CUDA_VISIBLE_DEVICES"] = "";
                hermesPsi.Environment["HIP_VISIBLE_DEVICES"] = "";
                hermesPsi.Environment["ROCR_VISIBLE_DEVICES"] = "";
                hermesPsi.Environment["HERMES_DASHBOARD_AUTOSTART"] = "0";

                using var hermes = Process.Start(hermesPsi)
                    ?? throw new InvalidOperationException("failed to start hermes");
                using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(queryTimeout));
                var stdoutTask = hermes.StandardOutput.ReadToEndAsync(cts.Token);
                var stderrTask = hermes.StandardError.ReadToEndAsync(cts.Token);
                await hermes.WaitForExitAsync(cts.Token);
                var output = (await stdoutTask).Trim();
                var error = (await stderrTask).Trim();
                var expected = manifest.GetProperty("hermes").GetProperty("probe_expected").GetString()!;
                var matched = ProbeMatched(output, expected);
                var finalLine = output.Split('\n').Select(l => l.Trim()).Reverse().FirstOrDefault(l => l.Length > 0) ?? "";
                return new Dictionary<string, object?>
                {
                    ["status"] = hermes.ExitCode == 0 && matched ? "pass" : "fail",
                    ["returncode"] = hermes.ExitCode,
                    ["expected"] = expected,
                    ["matched"] = matched,
                    ["final_line"] = finalLine,
                    ["output"] = output,
                    ["stderr"] = error,
                    ["base_url"] = baseUrl,
                    ["server_ready_seconds"] = readySeconds,
                    ["query_seconds"] = (DateTime.UtcNow - queryStarted).TotalSeconds,
                };
            }
            finally
            {
                try { hermesHome.Delete(true); } catch { /* ignore */ }
            }
        }
        finally
        {
            StopProcess(process);
            try { File.Delete(logPath); } catch { /* ignore */ }
        }
    }

    public static int RunCli(string[] args)
    {
        string? manifestPath = null, query = null, hermes = "hermes";
        double startupTimeout = 120, queryTimeout = -1;
        var dryRun = false;
        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--manifest": manifestPath = args[++i]; break;
                case "--query": query = args[++i]; break;
                case "--hermes": hermes = args[++i]; break;
                case "--startup-timeout": startupTimeout = double.Parse(args[++i]); break;
                case "--query-timeout": queryTimeout = double.Parse(args[++i]); break;
                case "--dry-run": dryRun = true; break;
                default: throw new ArgumentException($"unknown argument: {args[i]}");
            }
        }
        if (manifestPath is null)
            throw new ArgumentException("--manifest is required");
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(manifestPath));
            var model = ValidateManifest(doc.RootElement);
            query ??= doc.RootElement.GetProperty("hermes").GetProperty("default_probe").GetString()!;
            var port = doc.RootElement.GetProperty("runtime").TryGetProperty("port", out var p) && p.GetInt32() != 0
                ? p.GetInt32() : 18080;
            Dictionary<string, object?> result;
            if (dryRun)
            {
                result = new Dictionary<string, object?>
                {
                    ["status"] = "dry-run",
                    ["server_command"] = BuildServerCommand(doc.RootElement, model, port),
                    ["hermes_command"] = BuildHermesCommand(doc.RootElement, query, hermes),
                    ["cpu_only"] = true,
                    ["loopback"] = true,
                };
            }
            else
            {
                var qt = queryTimeout >= 0
                    ? queryTimeout
                    : doc.RootElement.GetProperty("hermes").GetProperty("query_timeout_seconds").GetDouble();
                result = RunWrapperAsync(doc.RootElement, model, query, hermes, startupTimeout, qt)
                    .GetAwaiter().GetResult();
            }
            Console.WriteLine(JsonSerializer.Serialize(result, JsonUtil.Indented));
            var status = Convert.ToString(result["status"]);
            return status is "pass" or "dry-run" ? 0 : 1;
        }
        catch (Exception ex) when (ex is IOException or ArgumentException or InvalidOperationException
                                       or TimeoutException or JsonException or OperationCanceledException)
        {
            Console.WriteLine(JsonSerializer.Serialize(new { status = "error", reason = ex.Message }, JsonUtil.Indented));
            return 1;
        }
    }
}
