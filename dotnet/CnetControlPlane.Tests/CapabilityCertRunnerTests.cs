using System.Text.Json;
using CnetControlPlane.Capability;

namespace CnetControlPlane.Tests;

public class CapabilityCertRunnerTests : IDisposable
{
    private readonly string _root;
    private readonly string _fixture;
    private const string FixtureSha = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

    public CapabilityCertRunnerTests()
    {
        _root = Directory.CreateTempSubdirectory("cnet-cert-test-").FullName;
        Directory.CreateDirectory(Path.Combine(_root, "fixtures"));
        Directory.CreateDirectory(Path.Combine(_root, "src"));
        File.WriteAllText(Path.Combine(_root, "src", "evaluator.c"), "int main(void){return 0;}\n");
        _fixture = Path.Combine(_root, "fixtures", "held_out.json");
        File.WriteAllText(_fixture, JsonSerializer.Serialize(new
        {
            capability_id = "test_capability",
            cases = new[] { new { id = "only-case", input = "unseen", expected = "pass" } },
            expected_markers = new[] { "TEST_PASS" },
        }));
    }

    public void Dispose() => Directory.Delete(_root, true);

    private string Manifest(Dictionary<string, object?>? overrides = null)
    {
        var path = Path.Combine(_root, "manifest.json");
        var value = new Dictionary<string, object?>
        {
            ["schema_version"] = 1,
            ["capability_id"] = "test_capability",
            ["held_out_fixture"] = "fixtures/held_out.json",
            ["evaluator"] = new List<object?> { "make", "test_capability" },
            ["evaluator_sources"] = new List<object?> { "src/evaluator.c" },
            ["metric_source"] = "heldout_receipt",
            ["required_marker"] = "TEST_PASS",
            ["title"] = "Test capability",
            ["owner"] = "test",
            ["evidence_artifact"] = "logs/test_capability.log",
            ["failure_envelope"] = "Fails if held-out behavior regresses.",
            ["absolute_floor"] = 1.0,
            ["baseline_metric"] = 1.0,
            ["regression_budget"] = 0.0,
        };
        if (overrides is not null)
        {
            foreach (var kv in overrides)
                value[kv.Key] = kv.Value;
        }
        File.WriteAllText(path, JsonSerializer.Serialize(value));
        return path;
    }

    private static string Receipt(
        string capability = "test_capability",
        string fixtureSha = FixtureSha,
        int cases = 1,
        int consumed = 1,
        string[]? caseIds = null,
        int reads = 2,
        int passed = 1,
        int errors = 0)
    {
        caseIds ??= ["only-case"];
        var lines = caseIds.Select(id => $"HELDOUT_CASE id={id} reads={reads}").ToList();
        lines.Add($"HELDOUT_FIXTURE capability={capability} sha256={fixtureSha} cases={cases} consumed={consumed}");
        var metric = cases == 0 ? 0 : (double)passed / cases;
        lines.Add($"HELDOUT_METRIC cases_passed={passed} cases_declared={cases} metric={metric:0.000000} errors={errors}");
        return string.Join('\n', lines) + "\nTEST_PASS\n";
    }

    [Fact]
    public void Valid_manifest_is_accepted()
    {
        var (manifest, fixturePath, fixture) = CapabilityCertRunner.ValidateManifest(_root, Manifest());
        Assert.Equal("test_capability", manifest["capability_id"]);
        Assert.Equal(_fixture, fixturePath);
        var cases = (List<object?>)fixture["cases"]!;
        Assert.Equal("unseen", ((Dictionary<string, object?>)cases[0]!)["input"]);
    }

    [Fact]
    public void Shell_evaluator_is_rejected()
    {
        var ex = Assert.Throws<ArgumentException>(() =>
            CapabilityCertRunner.ValidateManifest(_root, Manifest(new()
            {
                ["evaluator"] = new List<object?> { "sh", "-c", "touch /tmp/pwned" },
            })));
        Assert.Contains("non-shell", ex.Message);
    }

    [Fact]
    public void Evaluator_that_does_not_build_itself_must_declare_prepare()
    {
        var ex = Assert.Throws<ArgumentException>(() =>
            CapabilityCertRunner.ValidateManifest(_root, Manifest(new()
            {
                ["evaluator"] = new List<object?> { "dotnet", "test", "project.csproj" },
            })));
        Assert.Contains("evaluator_prepare", ex.Message);
    }

    [Fact]
    public void Declared_prepare_makes_dotnet_evaluator_acceptable()
    {
        var (manifest, _, _) = CapabilityCertRunner.ValidateManifest(_root, Manifest(new()
        {
            ["evaluator"] = new List<object?> { "dotnet", "test", "project.csproj" },
            ["evaluator_prepare"] = new List<object?> { "dotnet", "build", "project.csproj" },
            ["evaluator_binary"] = "src/evaluator.c",
        }));
        var prepare = (List<object?>)manifest["evaluator_prepare"]!;
        Assert.Equal("dotnet", prepare[0]);
    }

    [Fact]
    public void Shell_prepare_is_rejected()
    {
        var ex = Assert.Throws<ArgumentException>(() =>
            CapabilityCertRunner.ValidateManifest(_root, Manifest(new()
            {
                ["evaluator_prepare"] = new List<object?> { "sh", "-c", "touch /tmp/pwned" },
                ["evaluator_binary"] = "src/evaluator.c",
            })));
        Assert.Contains("allowlisted", ex.Message);
    }

    [Fact]
    public void Fixture_path_cannot_escape_repository()
    {
        var ex = Assert.Throws<ArgumentException>(() =>
            CapabilityCertRunner.ValidateManifest(_root, Manifest(new()
            {
                ["held_out_fixture"] = "../outside.json",
            })));
        Assert.Contains("escapes repository", ex.Message);
    }

    [Fact]
    public void Case_without_stable_id_is_rejected()
    {
        File.WriteAllText(_fixture, """{"capability_id":"test_capability","cases":[{"input":"unseen"}],"expected_markers":["TEST_PASS"]}""");
        var ex = Assert.Throws<ArgumentException>(() => CapabilityCertRunner.ValidateManifest(_root, Manifest()));
        Assert.Contains("stable id", ex.Message);
    }

    [Fact]
    public void Duplicate_case_ids_are_rejected()
    {
        File.WriteAllText(_fixture, """{"capability_id":"test_capability","cases":[{"id":"a"},{"id":"a"}],"expected_markers":["TEST_PASS"]}""");
        var ex = Assert.Throws<ArgumentException>(() => CapabilityCertRunner.ValidateManifest(_root, Manifest()));
        Assert.Contains("unique", ex.Message);
    }

    [Fact]
    public void Metric_source_must_be_declared()
    {
        var ex = Assert.Throws<ArgumentException>(() =>
            CapabilityCertRunner.ValidateManifest(_root, Manifest(new() { ["metric_source"] = "guess" })));
        Assert.Contains("metric_source", ex.Message);
    }

    [Fact]
    public void Regex_metric_source_requires_a_regex()
    {
        var ex = Assert.Throws<ArgumentException>(() =>
            CapabilityCertRunner.ValidateManifest(_root, Manifest(new() { ["metric_source"] = "regex" })));
        Assert.Contains("requires metric_regex", ex.Message);
    }

    [Fact]
    public void Receipt_metric_source_forbids_a_second_number()
    {
        var ex = Assert.Throws<ArgumentException>(() =>
            CapabilityCertRunner.ValidateManifest(_root, Manifest(new() { ["metric_regex"] = "metric=([0-9.]+)" })));
        Assert.Contains("one number, one source", ex.Message);
    }

    [Fact]
    public void Empty_evaluator_sources_are_rejected()
    {
        var ex = Assert.Throws<ArgumentException>(() =>
            CapabilityCertRunner.ValidateManifest(_root, Manifest(new()
            {
                ["evaluator_sources"] = new List<object?>(),
            })));
        Assert.Contains("nonempty path list", ex.Message);
    }

    [Fact]
    public void Missing_evaluator_source_is_an_error()
    {
        var ex = Assert.Throws<ArgumentException>(() =>
            CapabilityCertRunner.SourceSetDigest(_root, ["src/does_not_exist.c"]));
        Assert.Contains("missing", ex.Message);
    }

    [Fact]
    public void Source_set_digest_changes_with_content()
    {
        var (first, _) = CapabilityCertRunner.SourceSetDigest(_root, ["src/evaluator.c"]);
        File.WriteAllText(Path.Combine(_root, "src", "evaluator.c"), "int main(void){return 1;}\n");
        var (second, _) = CapabilityCertRunner.SourceSetDigest(_root, ["src/evaluator.c"]);
        Assert.NotEqual(first, second);
    }

    [Fact]
    public void Receipt_accepts_a_fully_consumed_fixture()
    {
        var (ok, problems, detail) = CapabilityCertRunner.CheckReceipt(
            Receipt(), "test_capability", FixtureSha, ["only-case"]);
        Assert.True(ok, string.Join("; ", problems));
        Assert.Equal(1.0, Convert.ToDouble(detail["receipt_metric"]));
    }

    [Fact]
    public void Absent_receipt_is_refused()
    {
        var (ok, problems, _) = CapabilityCertRunner.CheckReceipt(
            "TEST_PASS metric=1.000\n", "test_capability", FixtureSha, ["only-case"]);
        Assert.False(ok);
        Assert.Contains("no HELDOUT_FIXTURE receipt", problems[0]);
    }

    [Fact]
    public void Receipt_for_another_fixture_is_refused()
    {
        var (ok, problems, _) = CapabilityCertRunner.CheckReceipt(
            Receipt(fixtureSha: new string('b', 64)), "test_capability", FixtureSha, ["only-case"]);
        Assert.False(ok);
        Assert.Contains(problems, p => p.Contains("binds fixture"));
    }

    [Fact]
    public void Unconsumed_case_is_refused()
    {
        var (ok, problems, _) = CapabilityCertRunner.CheckReceipt(
            Receipt(consumed: 0, reads: 0, passed: 0), "test_capability", FixtureSha, ["only-case"]);
        Assert.False(ok);
        Assert.Contains(problems, p => p.Contains("never read"));
    }

    [Fact]
    public void Secret_env_values_never_reach_the_report()
    {
        const string sentinel = "SENTINEL-2f9a4c1e-DO-NOT-LEAK";
        var binding = CapabilityCertRunner.EnvironmentBinding(new Dictionary<string, string>
        {
            ["CNET_RESIDUAL_TOKEN"] = sentinel,
            ["CCE_API_KEY"] = sentinel + "-2",
            ["CNET_HELD_OUT_FIXTURE"] = "tests/fixtures/x.json",
            ["PATH"] = "/usr/bin",
        });
        var serialized = JsonSerializer.Serialize(binding);
        Assert.DoesNotContain(sentinel, serialized);
        Assert.DoesNotContain(sentinel + "-2", serialized);
        Assert.Contains("CNET_RESIDUAL_TOKEN", (List<string>)binding["env_knob_names"]!);
        Assert.StartsWith("sha256:", ((Dictionary<string, string>)binding["env_redacted_knobs"]!)["CNET_RESIDUAL_TOKEN"]);
    }

    [Fact]
    public void Allowlisted_knobs_keep_their_values()
    {
        var binding = CapabilityCertRunner.EnvironmentBinding(new Dictionary<string, string>
        {
            ["CNET_HELD_OUT_FIXTURE"] = "tests/fixtures/x.json",
        });
        Assert.Equal(
            "tests/fixtures/x.json",
            ((Dictionary<string, string>)binding["env_allowlisted_knobs"]!)["CNET_HELD_OUT_FIXTURE"]);
    }

    [Fact]
    public void Exact_marker_line_is_accepted()
    {
        var (found, problems) = CapabilityCertRunner.TerminalMarkerOk("CAP_X_PASS\n", "CAP_X_PASS");
        Assert.True(found, string.Join("; ", problems));
    }

    [Fact]
    public void Substring_predicate_does_not_certify()
    {
        const string output = "NOT_CAP_X_PASSED_YET waiting for the real gate\n";
        Assert.Contains("CAP_X_PASS", output);
        var (found, _) = CapabilityCertRunner.TerminalMarkerOk(output, "CAP_X_PASS");
        Assert.False(found);
    }

    [Fact]
    public void Duplicate_marker_lines_are_refused()
    {
        var (found, problems) = CapabilityCertRunner.TerminalMarkerOk("CAP_X_PASS\nCAP_X_PASS\n", "CAP_X_PASS");
        Assert.False(found);
        Assert.Contains(problems, p => p.Contains("2 times"));
    }

    [Fact]
    public void Conflicting_terminal_verdict_is_refused()
    {
        var (found, problems) = CapabilityCertRunner.TerminalMarkerOk("CAP_X_PASS\nCAP_X_FAIL\n", "CAP_X_PASS");
        Assert.False(found);
        Assert.Contains(problems, p => p.Contains("CAP_X_FAIL"));
    }

    [Fact]
    public void Two_fixture_receipts_are_refused()
    {
        var (ok, problems, _) = CapabilityCertRunner.CheckReceipt(
            Receipt() + Receipt(), "test_capability", FixtureSha, ["only-case"]);
        Assert.False(ok);
        Assert.Contains(problems, p => p.Contains("exactly one is a proof"));
    }

    [Fact]
    public void Metric_regex_matching_twice_is_refused()
    {
        var ex = Assert.Throws<ArgumentException>(() =>
            CapabilityCertRunner.ExtractMetric(
                new Dictionary<string, object?> { ["metric_source"] = "regex", ["metric_regex"] = @"acc_on=([0-9.]+)" },
                "acc_on=0.7375\nacc_on=0.9999\n",
                new Dictionary<string, object?>()));
        Assert.Contains("matched 2 times", ex.Message);
    }

    [Fact]
    public void Metric_regex_matching_once_is_accepted()
    {
        var value = CapabilityCertRunner.ExtractMetric(
            new Dictionary<string, object?> { ["metric_source"] = "regex", ["metric_regex"] = @"acc_on=([0-9.]+)" },
            "JTC_ADAPTER_BENCH_PASS acc_off=0.2975 acc_on=0.7375\n",
            new Dictionary<string, object?>());
        Assert.Equal(0.7375, value);
    }

    [Fact]
    public void Metric_comes_from_the_receipt_not_a_default()
    {
        var (_, _, detail) = CapabilityCertRunner.CheckReceipt(
            Receipt(cases: 2, consumed: 2, passed: 1, caseIds: ["a", "b"]),
            "test_capability", FixtureSha, ["a", "b"]);
        var metric = CapabilityCertRunner.ExtractMetric(
            new Dictionary<string, object?> { ["metric_source"] = "heldout_receipt" }, "", detail);
        Assert.Equal(0.5, metric);
    }

    [Fact]
    public void Receipt_metric_without_a_receipt_is_an_error()
    {
        var ex = Assert.Throws<ArgumentException>(() =>
            CapabilityCertRunner.ExtractMetric(
                new Dictionary<string, object?> { ["metric_source"] = "heldout_receipt" }, "",
                new Dictionary<string, object?>()));
        Assert.Contains("no receipt metric", ex.Message);
    }

    [Theory]
    [InlineData("acc_on=", false)]
    [InlineData("semantic=2", false)]
    [InlineData("CAP_X_PASS", true)]
    [InlineData("CAP_X_FAIL", true)]
    [InlineData("CAP_X_WITHHELD", true)]
    public void Looks_terminal_matches_verdict_shape(string marker, bool expected)
    {
        Assert.Equal(expected, CapabilityCertRunner.LooksTerminal(marker));
    }
}
