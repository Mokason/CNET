using System.Security.Cryptography;
using System.Text.Json;
using CnetControlPlane.Hermes;

namespace CnetControlPlane.Tests;

public class HermesTests
{
    private static Dictionary<string, object?> SampleReport(string root)
    {
        var selected = Path.Combine(root, "selected.gguf");
        File.WriteAllBytes(selected, "GGUFmodel"u8.ToArray());
        return new Dictionary<string, object?>
        {
            ["schema_version"] = 1,
            ["overall_pass"] = true,
            ["verdict"] = "PASS_CANDIDATE_QUARANTINED",
            ["artifacts"] = new Dictionary<string, object?>
            {
                ["reference"] = new Dictionary<string, object?>
                {
                    ["path"] = selected,
                    ["sha256"] = "abc",
                    ["bytes"] = new FileInfo(selected).Length,
                },
                ["candidate"] = new Dictionary<string, object?>
                {
                    ["path"] = Path.Combine(root, "bad.gguf"),
                    ["sha256"] = "def",
                    ["bytes"] = 5,
                },
                ["qgkp"] = new Dictionary<string, object?>
                {
                    ["path"] = Path.Combine(root, "source.qgkp"),
                    ["round_trip"] = new Dictionary<string, object?> { ["byte_identical"] = true },
                },
            },
            ["admission"] = new Dictionary<string, object?>
            {
                ["admitted"] = false,
                ["selected_role"] = "reference",
                ["reasons"] = new List<object?> { "quality_regression" },
            },
            ["selected_model"] = new Dictionary<string, object?>
            {
                ["role"] = "reference",
                ["path"] = selected,
            },
            ["restart_integrity"] = new Dictionary<string, object?>
            {
                ["responses_identical"] = true,
                ["quality_preserved"] = true,
            },
        };
    }

    private static JsonElement ToElement(object value)
    {
        var json = JsonSerializer.Serialize(value);
        return JsonDocument.Parse(json).RootElement.Clone();
    }

    [Fact]
    public void Manifest_selects_only_admitted_or_fallback_artifact()
    {
        var root = Directory.CreateTempSubdirectory("cnet-hermes-").FullName;
        try
        {
            var report = SampleReport(root);
            var manifest = HermesWrapperRenderer.BuildManifest(ToElement(report), "report.json", "/bin/llama-server");
            var selected = (Dictionary<string, object?>)manifest["selected_model"]!;
            Assert.Equal("reference", selected["role"]);
            Assert.Equal(((Dictionary<string, object?>)report["selected_model"]!)["path"], selected["path"]);
            var runtime = (Dictionary<string, object?>)manifest["runtime"]!;
            Assert.True(Convert.ToBoolean(runtime["cpu_only"]));
            Assert.True(Convert.ToInt32(runtime["ctx_size"]) >= 65536);
            var hermes = (Dictionary<string, object?>)manifest["hermes"]!;
            Assert.Equal("custom", hermes["provider"]);
            Assert.Equal("Ready", hermes["probe_expected"]);
            Assert.Contains("qwen3.5", Convert.ToString(hermes["model"])!, StringComparison.OrdinalIgnoreCase);
            Assert.True(Convert.ToInt32(hermes["max_output_tokens"]) <= 256);
            Assert.InRange(Convert.ToInt32(hermes["query_timeout_seconds"]), 180, 600);
            Assert.Contains("run-hermes-wrapper", Convert.ToString(hermes["start_command"])!);
        }
        finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void Failed_campaign_cannot_produce_wrapper()
    {
        var root = Directory.CreateTempSubdirectory("cnet-hermes-fail-").FullName;
        try
        {
            var report = SampleReport(root);
            report["overall_pass"] = false;
            Assert.Throws<ArgumentException>(() =>
                HermesWrapperRenderer.BuildManifest(ToElement(report), "report.json", "/bin/llama-server"));
        }
        finally { Directory.Delete(root, true); }
    }

    private static JsonElement Manifest(string root)
    {
        var model = Path.Combine(root, "selected.gguf");
        File.WriteAllBytes(model, "GGUFmodel"u8.ToArray());
        var digest = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(model))).ToLowerInvariant();
        return ToElement(new Dictionary<string, object?>
        {
            ["schema_version"] = 1,
            ["kind"] = "cnet-hermes-wrapper",
            ["selected_model"] = new Dictionary<string, object?>
            {
                ["role"] = "reference",
                ["path"] = model,
                ["sha256"] = digest,
            },
            ["runtime"] = new Dictionary<string, object?>
            {
                ["backend"] = "llama-server",
                ["server"] = "/bin/llama-server",
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
                ["model"] = "qwen/qwen3.5-test",
                ["default_probe"] = "Reply with exactly Ready",
                ["probe_expected"] = "Ready",
                ["max_output_tokens"] = 64,
                ["max_turns"] = 2,
                ["query_timeout_seconds"] = 540,
            },
        });
    }

    [Fact]
    public void Manifest_validation_is_hash_and_cpu_fail_closed()
    {
        var root = Directory.CreateTempSubdirectory("cnet-run-hermes-").FullName;
        try
        {
            using var doc = JsonDocument.Parse(Manifest(root).GetRawText());
            var selected = HermesWrapperRunner.ValidateManifest(doc.RootElement);
            Assert.Equal("selected.gguf", Path.GetFileName(selected));
            var mutable = JsonSerializer.Deserialize<Dictionary<string, JsonElement>>(doc.RootElement.GetRawText())!;
            // Rebuild with gpu_layers=1
            var bad = JsonDocument.Parse(doc.RootElement.GetRawText().Replace("\"gpu_layers\":0", "\"gpu_layers\":1"));
            Assert.Throws<ArgumentException>(() => HermesWrapperRunner.ValidateManifest(bad.RootElement));
            var badCtx = JsonDocument.Parse(doc.RootElement.GetRawText().Replace("\"ctx_size\":65536", "\"ctx_size\":8192"));
            Assert.Throws<ArgumentException>(() => HermesWrapperRunner.ValidateManifest(badCtx.RootElement));
        }
        finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void Hash_mismatch_is_rejected()
    {
        var root = Directory.CreateTempSubdirectory("cnet-hash-").FullName;
        try
        {
            var json = Manifest(root).GetRawText().Replace(
                Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(Path.Combine(root, "selected.gguf")))).ToLowerInvariant(),
                new string('0', 64));
            using var doc = JsonDocument.Parse(json);
            Assert.Throws<ArgumentException>(() => HermesWrapperRunner.ValidateManifest(doc.RootElement));
        }
        finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void Commands_pin_cpu_and_custom_provider()
    {
        var root = Directory.CreateTempSubdirectory("cnet-cmd-").FullName;
        try
        {
            using var doc = JsonDocument.Parse(Manifest(root).GetRawText());
            var model = HermesWrapperRunner.ValidateManifest(doc.RootElement);
            var server = HermesWrapperRunner.BuildServerCommand(doc.RootElement, model, 18888);
            Assert.Contains("--n-gpu-layers", server);
            Assert.Equal("0", server[server.IndexOf("--n-gpu-layers") + 1]);
            Assert.Contains("--jinja", server);
            Assert.Equal("{\"enable_thinking\":false}", server[server.IndexOf("--chat-template-kwargs") + 1]);
            var hermes = HermesWrapperRunner.BuildHermesCommand(doc.RootElement, "probe");
            Assert.Equal("custom", hermes[hermes.IndexOf("--provider") + 1]);
            Assert.Equal("2", hermes[hermes.IndexOf("--max-turns") + 1]);
            Assert.Contains("--ignore-rules", hermes);
            Assert.DoesNotContain("--ignore-user-config", hermes);
            var config = HermesWrapperRunner.BuildHermesConfig(doc.RootElement, "http://127.0.0.1:18888/v1");
            var modelCfg = (Dictionary<string, object?>)config["model"]!;
            Assert.Equal(64, Convert.ToInt32(modelCfg["max_tokens"]));
            Assert.False(config.ContainsKey("max_tokens"));
            Assert.Equal(65536, Convert.ToInt32(modelCfg["context_length"]));
            Assert.Equal("http://127.0.0.1:18888/v1", modelCfg["base_url"]);
        }
        finally { Directory.Delete(root, true); }
    }

    [Fact]
    public void Probe_match_requires_clean_final_line()
    {
        Assert.True(HermesWrapperRunner.ProbeMatched("warning\nReady", "Ready"));
        Assert.False(HermesWrapperRunner.ProbeMatched("Reasoning mentions Ready\nnot final", "Ready"));
        Assert.False(HermesWrapperRunner.ProbeMatched("Ready\nReached maximum iterations (2)", "Ready"));
    }
}
