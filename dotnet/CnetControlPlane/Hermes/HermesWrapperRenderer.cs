using System.Text.Json;
using System.Text.Json.Nodes;

namespace CnetControlPlane.Hermes;

public static class HermesWrapperRenderer
{
    public static Dictionary<string, object?> BuildManifest(
        JsonElement report,
        string reportPath,
        string server)
    {
        if (!report.TryGetProperty("overall_pass", out var overall) || overall.ValueKind != JsonValueKind.True)
            throw new ArgumentException("acceptance report is not green");
        var restart = report.TryGetProperty("restart_integrity", out var ri) ? ri : default;
        if (restart.ValueKind != JsonValueKind.Object
            || !restart.TryGetProperty("responses_identical", out var resp) || resp.ValueKind != JsonValueKind.True
            || !restart.TryGetProperty("quality_preserved", out var qp) || qp.ValueKind != JsonValueKind.True)
        {
            throw new ArgumentException("selected model did not survive restart");
        }

        var selected = report.TryGetProperty("selected_model", out var sel) ? sel : default;
        var role = selected.ValueKind == JsonValueKind.Object && selected.TryGetProperty("role", out var r)
            ? r.GetString() : null;
        if (role is not ("reference" or "candidate"))
            throw new ArgumentException($"invalid selected model role: {role}");

        var artifact = report.TryGetProperty("artifacts", out var arts)
            && arts.TryGetProperty(role, out var art) ? art : default;
        var selectedPath = selected.TryGetProperty("path", out var sp) ? sp.GetString() : null;
        var artifactPath = artifact.ValueKind == JsonValueKind.Object && artifact.TryGetProperty("path", out var ap)
            ? ap.GetString() : null;
        if (selectedPath != artifactPath)
            throw new ArgumentException("selected model path does not match campaign artifact");
        if (artifact.ValueKind != JsonValueKind.Object
            || !artifact.TryGetProperty("sha256", out var sha)
            || string.IsNullOrEmpty(sha.GetString()))
        {
            throw new ArgumentException("selected artifact lacks SHA-256 evidence");
        }

        return new Dictionary<string, object?>
        {
            ["schema_version"] = 1,
            ["kind"] = "cnet-hermes-wrapper",
            ["source_report"] = Path.GetFullPath(reportPath),
            ["campaign_verdict"] = report.TryGetProperty("verdict", out var v) ? v.GetString() : null,
            ["selected_model"] = new Dictionary<string, object?>
            {
                ["role"] = role,
                ["path"] = selectedPath,
                ["sha256"] = sha.GetString(),
                ["bytes"] = artifact.TryGetProperty("bytes", out var b) ? JsonUtil.ToClr(b) : null,
            },
            ["candidate_admission"] = report.TryGetProperty("admission", out var adm)
                ? JsonUtil.ToDict(adm) : new Dictionary<string, object?>(),
            ["runtime"] = new Dictionary<string, object?>
            {
                ["backend"] = "llama-server",
                ["server"] = server,
                ["cpu_only"] = true,
                ["host"] = "127.0.0.1",
                ["port"] = 0,
                ["threads"] = 4,
                ["ctx_size"] = 65536,
                ["parallel"] = 1,
                ["gpu_layers"] = 0,
            },
            ["hermes"] = new Dictionary<string, object?>
            {
                ["provider"] = "custom",
                ["model"] = "qwen/qwen3.5-9b-cnet-selected",
                ["api_mode"] = "chat_completions",
                ["default_probe"] = "Reply with exactly Ready",
                ["probe_expected"] = "Ready",
                ["max_output_tokens"] = 64,
                ["max_turns"] = 2,
                ["query_timeout_seconds"] = 540,
                ["start_command"] =
                    "dotnet run --project dotnet/CnetControlPlane -- run-hermes-wrapper --manifest <manifest.json>",
            },
        };
    }

    public static int RunCli(string[] args)
    {
        string? report = null, server = null, output = null;
        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--report": report = args[++i]; break;
                case "--server": server = args[++i]; break;
                case "--output": output = args[++i]; break;
                default: throw new ArgumentException($"unknown argument: {args[i]}");
            }
        }
        if (report is null || server is null || output is null)
            throw new ArgumentException("--report, --server, and --output are required");
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(report));
            var manifest = BuildManifest(doc.RootElement, report, Path.GetFullPath(server));
            var dir = Path.GetDirectoryName(output);
            if (!string.IsNullOrEmpty(dir))
                Directory.CreateDirectory(dir);
            File.WriteAllText(output, JsonSerializer.Serialize(manifest, JsonUtil.Indented) + "\n");
            var selected = (Dictionary<string, object?>)manifest["selected_model"]!;
            Console.WriteLine(JsonSerializer.Serialize(new
            {
                status = "written",
                manifest = output,
                selected_role = selected["role"],
            }));
            return 0;
        }
        catch (Exception ex) when (ex is IOException or ArgumentException or JsonException)
        {
            Console.WriteLine(JsonSerializer.Serialize(new { status = "refused", reason = ex.Message }));
            return 1;
        }
    }
}
