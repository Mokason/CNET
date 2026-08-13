using System.Text.Json;
using System.Text.RegularExpressions;
using CnetControlPlane.Capability;

namespace CnetControlPlane.Tests;

/// <summary>
/// Core fixture-causality rules: every committed capability has declared mutations
/// covering every field name and case (from test_capability_fixture_causality.py).
/// Full evaluator mutation runs remain a Makefile-wired integration gate.
/// </summary>
public class FixtureCausalityCoreTests
{
    private static readonly Dictionary<string, List<(int Index, string Key, JsonElement Value)>> Mutations = new()
    {
        ["calibrated_abstention"] =
        [
            (0, "expected", default), (0, "margin", default), (1, "expected", default),
            (1, "reliability", default), (1, "samples", default), (2, "expected", default),
            (2, "evidence_refs", default),
        ],
        ["cce_classification"] =
        [
            (0, "minimum_accuracy", default), (0, "classes", default), (0, "held_out_samples", default),
            (0, "diff_mode", default), (0, "gradient_clip", default), (0, "label_rule", default),
            (0, "minimum_distinct_classes", default), (0, "minimum_lift_over_majority", default),
        ],
        ["honest_memory_retrieval"] =
        [
            (0, "expected", default), (0, "stored", default), (0, "query", default),
        ],
        ["hybrid_skill_serve"] =
        [
            (0, "expected_authority", default), (0, "backend", default), (0, "query", default),
            (0, "expected_proposals", default), (1, "expected_authority", default), (1, "backend", default),
        ],
        ["json_toolcall_adapter"] =
        [
            (0, "minimum_accuracy_with_adapter", default), (0, "adapter_on_baseline", default),
            (0, "adapter_off_baseline", default), (0, "baseline_tolerance", default),
            (0, "metric", default), (0, "skill", default), (0, "held_out_pairs_from", default),
            (0, "certify_before_serve", default),
        ],
        ["sleep_consolidation"] =
        [
            (0, "expected_semantic_promotions", default), (0, "expected_procedural_promotions", default),
            (0, "episodes", default), (0, "duplicate_episodes", default), (0, "minimum_pruned", default),
            (0, "minimum_merges", default), (0, "minimum_graduated", default),
        ],
    };

    [Fact]
    public void Every_committed_capability_has_mutation_coverage_for_declared_fields()
    {
        var root = CnetControlPlane.RepoPaths.FindRoot();
        var manifestDir = Path.Combine(root, "config", "capability_manifests");
        foreach (var manifestPath in Directory.GetFiles(manifestDir, "*.json").OrderBy(x => x))
        {
            using var manifestDoc = JsonDocument.Parse(File.ReadAllText(manifestPath));
            var capabilityId = manifestDoc.RootElement.GetProperty("capability_id").GetString()!;
            Assert.True(Mutations.ContainsKey(capabilityId), $"{capabilityId}: no declared semantic mutation");
            var fixtureRel = manifestDoc.RootElement.GetProperty("held_out_fixture").GetString()!;
            using var fixtureDoc = JsonDocument.Parse(File.ReadAllText(Path.Combine(root, fixtureRel)));
            var cases = fixtureDoc.RootElement.GetProperty("cases");
            var declaredNames = new HashSet<string>();
            var declaredCases = new HashSet<int>();
            for (var i = 0; i < cases.GetArrayLength(); i++)
            {
                declaredCases.Add(i);
                foreach (var prop in cases[i].EnumerateObject())
                {
                    if (prop.Name != "id")
                        declaredNames.Add(prop.Name);
                }
            }
            var coveredNames = Mutations[capabilityId].Select(m => m.Key).ToHashSet();
            var coveredCases = Mutations[capabilityId].Select(m => m.Index).ToHashSet();
            Assert.Empty(declaredNames.Except(coveredNames));
            Assert.Empty(declaredCases.Except(coveredCases));
        }
    }

    [Fact]
    public void Receipt_regex_matches_heldout_fixture_line()
    {
        var re = new Regex(
            @"^HELDOUT_FIXTURE capability=(\S+) sha256=([0-9a-f]{64}) cases=(\d+) consumed=(\d+)$",
            RegexOptions.Multiline);
        var text = "HELDOUT_FIXTURE capability=demo sha256=" + new string('a', 64) + " cases=2 consumed=2\n";
        Assert.Matches(re, text);
    }

    [Fact]
    public void Missing_receipt_fails_closed()
    {
        var (ok, problems, _) = CapabilityCertRunner.CheckReceipt(
            "CAP_X_PASS\n", "demo", new string('a', 64), ["c1"]);
        Assert.False(ok);
        Assert.Contains(problems, p => p.Contains("no HELDOUT_FIXTURE receipt"));
    }
}
