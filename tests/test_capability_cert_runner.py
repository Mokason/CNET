#!/usr/bin/env python3
"""Unit gate for the capability certificate runner.

The runner is the thing that decides whether a capability is certified, so the
negatives are the point: a manifest that shells out, a fixture path that escapes
the repository, a case without a stable id, a metric with no declared source,
and — the defect this file grew for — an evaluator that emits no consumption
receipt, the wrong fixture digest, or a receipt covering fewer cases than the
fixture declares.
"""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


# CNET_CERT_RUNNER lets a RED run point these cases at the pre-fix runner
# without copying them anywhere.
RUNNER_PATH = Path(
    os.environ.get("CNET_CERT_RUNNER", "")
    or Path(__file__).with_name("run_capability_cert.py")
)
SPEC = importlib.util.spec_from_file_location("capability_cert_runner", RUNNER_PATH)
assert SPEC and SPEC.loader
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)

FIXTURE_SHA = "a" * 64


def receipt(
    capability: str = "test_capability",
    fixture_sha: str = FIXTURE_SHA,
    cases: int = 1,
    consumed: int = 1,
    case_ids: tuple[str, ...] = ("only-case",),
    reads: int = 2,
    passed: int = 1,
    errors: int = 0,
) -> str:
    lines = [f"HELDOUT_CASE id={case_id} reads={reads}" for case_id in case_ids]
    lines.append(
        f"HELDOUT_FIXTURE capability={capability} sha256={fixture_sha} "
        f"cases={cases} consumed={consumed}"
    )
    lines.append(
        f"HELDOUT_METRIC cases_passed={passed} cases_declared={cases} "
        f"metric={passed / cases if cases else 0:.6f} errors={errors}"
    )
    return "\n".join(lines) + "\nTEST_PASS\n"


class CapabilityCertRunnerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="cnet-cert-test-")
        self.root = Path(self.temporary.name).resolve()
        (self.root / "fixtures").mkdir()
        (self.root / "src").mkdir()
        (self.root / "src" / "evaluator.c").write_text("int main(void){return 0;}\n")
        self.fixture = self.root / "fixtures" / "held_out.json"
        self.fixture.write_text(
            json.dumps(
                {
                    "capability_id": "test_capability",
                    "cases": [
                        {"id": "only-case", "input": "unseen", "expected": "pass"}
                    ],
                    "expected_markers": ["TEST_PASS"],
                }
            ),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def manifest(self, **overrides) -> Path:
        path = self.root / "manifest.json"
        value = {
            "schema_version": 1,
            "capability_id": "test_capability",
            "held_out_fixture": "fixtures/held_out.json",
            "evaluator": ["make", "test_capability"],
            "evaluator_sources": ["src/evaluator.c"],
            "metric_source": "heldout_receipt",
            "required_marker": "TEST_PASS",
            "title": "Test capability",
            "owner": "test",
            "evidence_artifact": "logs/test_capability.log",
            "failure_envelope": "Fails if held-out behavior regresses.",
            "absolute_floor": 1.0,
            "baseline_metric": 1.0,
            "regression_budget": 0.0,
        }
        value.update(overrides)
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    # --- manifest validation ---------------------------------------------

    def test_valid_manifest_is_accepted(self) -> None:
        manifest, fixture_path, fixture = RUNNER.validate_manifest(
            self.root, self.manifest()
        )
        self.assertEqual(manifest["capability_id"], "test_capability")
        self.assertEqual(fixture_path, self.fixture)
        self.assertEqual(fixture["cases"][0]["input"], "unseen")

    def test_shell_evaluator_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "non-shell"):
            RUNNER.validate_manifest(
                self.root, self.manifest(evaluator=["sh", "-c", "touch /tmp/pwned"])
            )

    def test_fixture_path_cannot_escape_repository(self) -> None:
        with self.assertRaisesRegex(ValueError, "escapes repository"):
            RUNNER.validate_manifest(
                self.root, self.manifest(held_out_fixture="../outside.json")
            )

    def test_case_without_stable_id_is_rejected(self) -> None:
        self.fixture.write_text(
            json.dumps(
                {
                    "capability_id": "test_capability",
                    "cases": [{"input": "unseen"}],
                    "expected_markers": ["TEST_PASS"],
                }
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "stable id"):
            RUNNER.validate_manifest(self.root, self.manifest())

    def test_duplicate_case_ids_are_rejected(self) -> None:
        self.fixture.write_text(
            json.dumps(
                {
                    "capability_id": "test_capability",
                    "cases": [{"id": "a"}, {"id": "a"}],
                    "expected_markers": ["TEST_PASS"],
                }
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "unique"):
            RUNNER.validate_manifest(self.root, self.manifest())

    def test_metric_source_must_be_declared(self) -> None:
        with self.assertRaisesRegex(ValueError, "metric_source"):
            RUNNER.validate_manifest(self.root, self.manifest(metric_source="guess"))

    def test_regex_metric_source_requires_a_regex(self) -> None:
        with self.assertRaisesRegex(ValueError, "requires metric_regex"):
            RUNNER.validate_manifest(self.root, self.manifest(metric_source="regex"))

    def test_receipt_metric_source_forbids_a_second_number(self) -> None:
        with self.assertRaisesRegex(ValueError, "one number, one source"):
            RUNNER.validate_manifest(
                self.root, self.manifest(metric_regex="metric=([0-9.]+)")
            )

    def test_empty_evaluator_sources_are_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "nonempty path list"):
            RUNNER.validate_manifest(self.root, self.manifest(evaluator_sources=[]))

    def test_missing_evaluator_source_is_an_error(self) -> None:
        with self.assertRaisesRegex(ValueError, "missing"):
            RUNNER.source_set_digest(self.root, ["src/does_not_exist.c"])

    def test_source_set_digest_changes_with_content(self) -> None:
        first, _ = RUNNER.source_set_digest(self.root, ["src/evaluator.c"])
        (self.root / "src" / "evaluator.c").write_text("int main(void){return 1;}\n")
        second, _ = RUNNER.source_set_digest(self.root, ["src/evaluator.c"])
        self.assertNotEqual(first, second)

    # --- receipt enforcement ---------------------------------------------

    def test_receipt_accepts_a_fully_consumed_fixture(self) -> None:
        ok, problems, detail = RUNNER.check_receipt(
            receipt(), "test_capability", FIXTURE_SHA, ["only-case"]
        )
        self.assertTrue(ok, problems)
        self.assertEqual(detail["receipt_metric"], 1.0)

    def test_absent_receipt_is_refused(self) -> None:
        ok, problems, _ = RUNNER.check_receipt(
            "TEST_PASS metric=1.000\n", "test_capability", FIXTURE_SHA, ["only-case"]
        )
        self.assertFalse(ok)
        self.assertIn("no HELDOUT_FIXTURE receipt", problems[0])

    def test_receipt_for_another_fixture_is_refused(self) -> None:
        ok, problems, _ = RUNNER.check_receipt(
            receipt(fixture_sha="b" * 64),
            "test_capability",
            FIXTURE_SHA,
            ["only-case"],
        )
        self.assertFalse(ok)
        self.assertTrue(any("binds fixture" in problem for problem in problems))

    def test_receipt_for_another_capability_is_refused(self) -> None:
        ok, problems, _ = RUNNER.check_receipt(
            receipt(capability="other"), "test_capability", FIXTURE_SHA, ["only-case"]
        )
        self.assertFalse(ok)
        self.assertTrue(any("names capability" in problem for problem in problems))

    def test_unconsumed_case_is_refused(self) -> None:
        ok, problems, _ = RUNNER.check_receipt(
            receipt(consumed=0, reads=0, passed=0),
            "test_capability",
            FIXTURE_SHA,
            ["only-case"],
        )
        self.assertFalse(ok)
        self.assertTrue(any("never read" in problem for problem in problems))

    def test_missing_case_is_refused(self) -> None:
        ok, problems, _ = RUNNER.check_receipt(
            receipt(cases=2, consumed=2, passed=2),
            "test_capability",
            FIXTURE_SHA,
            ["only-case", "second-case"],
        )
        self.assertFalse(ok)
        self.assertTrue(any("absent from the receipt" in p for p in problems))

    def test_read_errors_are_refused(self) -> None:
        ok, problems, _ = RUNNER.check_receipt(
            receipt(errors=2), "test_capability", FIXTURE_SHA, ["only-case"]
        )
        self.assertFalse(ok)
        self.assertTrue(any("read errors" in problem for problem in problems))

    def test_failed_case_is_refused(self) -> None:
        ok, problems, _ = RUNNER.check_receipt(
            receipt(passed=0), "test_capability", FIXTURE_SHA, ["only-case"]
        )
        self.assertFalse(ok)
        self.assertTrue(any("cases passed" in problem for problem in problems))

    # --- environment redaction -------------------------------------------

    def test_secret_env_values_never_reach_the_report(self) -> None:
        sentinel = "SENTINEL-2f9a4c1e-DO-NOT-LEAK"
        binding = RUNNER.environment_binding(
            {
                "CNET_RESIDUAL_TOKEN": sentinel,
                "CCE_API_KEY": sentinel + "-2",
                "CNET_HELD_OUT_FIXTURE": "tests/fixtures/x.json",
                "PATH": "/usr/bin",
            }
        )
        serialized = json.dumps(binding)
        self.assertNotIn(sentinel, serialized)
        self.assertNotIn(sentinel + "-2", serialized)
        # The names are still there: a knob that appeared or vanished must be
        # visible, only its value is withheld.
        self.assertIn("CNET_RESIDUAL_TOKEN", binding["env_knob_names"])
        self.assertIn("CCE_API_KEY", binding["env_knob_names"])
        self.assertTrue(
            binding["env_redacted_knobs"]["CNET_RESIDUAL_TOKEN"].startswith("sha256:")
        )

    def test_allowlisted_knobs_keep_their_values(self) -> None:
        binding = RUNNER.environment_binding(
            {"CNET_HELD_OUT_FIXTURE": "tests/fixtures/x.json"}
        )
        self.assertEqual(
            binding["env_allowlisted_knobs"]["CNET_HELD_OUT_FIXTURE"],
            "tests/fixtures/x.json",
        )

    def test_a_changed_secret_still_changes_the_binding(self) -> None:
        first = RUNNER.environment_binding({"CNET_RESIDUAL_TOKEN": "a"})
        second = RUNNER.environment_binding({"CNET_RESIDUAL_TOKEN": "b"})
        self.assertNotEqual(first["env_sha256"], second["env_sha256"])
        self.assertNotEqual(
            first["env_redacted_knobs"]["CNET_RESIDUAL_TOKEN"],
            second["env_redacted_knobs"]["CNET_RESIDUAL_TOKEN"],
        )

    # --- pre/post state binding -------------------------------------------

    def git_root(self) -> Path:
        """A throwaway git repo, so tracked/dirty/untracked bindings are real."""
        root = self.root
        env = dict(os.environ, GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null")
        def run(*args: str) -> None:
            subprocess.run(["git", *args], cwd=root, env=env, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        run("init", "-q")
        run("config", "user.email", "t@example.invalid")
        run("config", "user.name", "test")
        run("add", "-A")
        run("commit", "-qm", "fixture")
        return root

    def state(self, include_binary: bool = True) -> dict:
        return RUNNER.capture_state(
            self.root, ["src/evaluator.c"], self.fixture, "src/evaluator.c",
            include_binary,
        )

    def test_binding_is_stable_when_nothing_moves(self) -> None:
        self.git_root()
        self.assertEqual(RUNNER.compare_states(self.state(), self.state()), [])

    def test_source_mutation_after_the_receipt_is_detected(self) -> None:
        self.git_root()
        before = self.state()
        (self.root / "src" / "evaluator.c").write_text("int main(void){return 2;}\n")
        drift = RUNNER.compare_states(before, self.state())
        self.assertIn("evaluator_source_set_sha256", drift)

    def test_fixture_mutation_after_the_receipt_is_detected(self) -> None:
        self.git_root()
        before = self.state()
        self.fixture.write_text(self.fixture.read_text() + "\n", encoding="utf-8")
        drift = RUNNER.compare_states(before, self.state())
        self.assertIn("fixture_sha256", drift)

    def test_binary_mutation_after_the_receipt_is_detected(self) -> None:
        self.git_root()
        before = self.state()
        (self.root / "src" / "evaluator.c").write_text("swapped\n")
        drift = RUNNER.compare_states(before, self.state())
        self.assertIn("evaluator_binary_sha256", drift)

    def test_tracked_dirty_content_mutation_is_detected(self) -> None:
        self.git_root()
        other = self.root / "unrelated.txt"
        other.write_text("one\n")
        before = self.state()
        other.write_text("two\n")      # same path, same status text, new bytes
        drift = RUNNER.compare_states(before, self.state())
        self.assertIn("untracked_sha256", drift)

    def test_committed_file_content_mutation_is_detected(self) -> None:
        root = self.git_root()
        tracked = root / "tracked.txt"
        tracked.write_text("one\n")
        subprocess.run(["git", "add", "tracked.txt"], cwd=root, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(
            ["git", "-c", "user.email=t@example.invalid", "-c", "user.name=test",
             "commit", "-qm", "tracked"],
            cwd=root, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        before = self.state()
        tracked.write_text("two\n")
        drift = RUNNER.compare_states(before, self.state())
        self.assertIn("worktree_sha256", drift)

    def test_untracked_file_appearing_is_detected(self) -> None:
        self.git_root()
        before = self.state()
        (self.root / "src" / "smuggled.c").write_text("int extra(void){return 0;}\n")
        drift = RUNNER.compare_states(before, self.state())
        self.assertIn("untracked_sha256", drift)

    # --- receipt cardinality ---------------------------------------------

    def test_two_fixture_receipts_are_refused(self) -> None:
        ok, problems, _ = RUNNER.check_receipt(
            receipt() + receipt(), "test_capability", FIXTURE_SHA, ["only-case"]
        )
        self.assertFalse(ok)
        self.assertTrue(any("exactly one is a proof" in p for p in problems))

    def test_two_metric_receipts_are_refused(self) -> None:
        text = receipt() + (
            "HELDOUT_METRIC cases_passed=1 cases_declared=1 "
            "metric=1.000000 errors=0\n"
        )
        ok, problems, _ = RUNNER.check_receipt(
            text, "test_capability", FIXTURE_SHA, ["only-case"]
        )
        self.assertFalse(ok)
        self.assertTrue(any("HELDOUT_METRIC lines" in p for p in problems))

    def test_duplicate_case_line_is_refused(self) -> None:
        text = receipt(case_ids=("only-case", "only-case"))
        ok, problems, _ = RUNNER.check_receipt(
            text, "test_capability", FIXTURE_SHA, ["only-case"]
        )
        self.assertFalse(ok)
        self.assertTrue(any("more than once" in p for p in problems))

    def test_conflicting_duplicate_case_lines_are_refused(self) -> None:
        text = (
            "HELDOUT_CASE id=only-case reads=3\n"
            "HELDOUT_CASE id=only-case reads=0\n"
            "HELDOUT_FIXTURE capability=test_capability "
            f"sha256={FIXTURE_SHA} cases=1 consumed=1\n"
            "HELDOUT_METRIC cases_passed=1 cases_declared=1 metric=1.000000 errors=0\n"
        )
        ok, problems, _ = RUNNER.check_receipt(
            text, "test_capability", FIXTURE_SHA, ["only-case"]
        )
        self.assertFalse(ok)
        self.assertTrue(any("more than once" in p for p in problems))

    def test_undeclared_case_line_is_refused(self) -> None:
        text = receipt(case_ids=("only-case", "smuggled-case"))
        ok, problems, _ = RUNNER.check_receipt(
            text, "test_capability", FIXTURE_SHA, ["only-case"]
        )
        self.assertFalse(ok)
        self.assertTrue(
            any("which the fixture does not declare" in p for p in problems)
        )

    def test_metric_regex_matching_twice_is_refused(self) -> None:
        with self.assertRaisesRegex(ValueError, "matched 2 times"):
            RUNNER.extract_metric(
                {"metric_source": "regex", "metric_regex": r"acc_on=([0-9.]+)"},
                "acc_on=0.7375\nacc_on=0.9999\n",
                {},
            )

    def test_metric_regex_matching_once_is_accepted(self) -> None:
        value = RUNNER.extract_metric(
            {"metric_source": "regex", "metric_regex": r"acc_on=([0-9.]+)"},
            "JTC_ADAPTER_BENCH_PASS acc_off=0.2975 acc_on=0.7375\n",
            {},
        )
        self.assertEqual(value, 0.7375)

    def test_metric_regex_matching_never_is_refused(self) -> None:
        with self.assertRaisesRegex(ValueError, "did not match"):
            RUNNER.extract_metric(
                {"metric_source": "regex", "metric_regex": r"acc_on=([0-9.]+)"},
                "nothing here\n",
                {},
            )

    # --- terminal marker cardinality --------------------------------------
    #
    # The runner accepted `marker in stdout`, a raw substring test. That makes
    # `X_PASSED`, `NOT_X_PASS` and a mid-sentence mention all count as the
    # verdict `X_PASS`, counts no duplicates, and lets a log asserting both
    # `X_PASS` and `X_FAIL` certify. `gate_evidence.py` already solved this;
    # the certificate runner is the stricter of the two and had the weaker test.

    def test_the_substring_predicate_this_replaced_accepted_a_non_verdict(self) -> None:
        """A standing witness for the defect, not just for its absence.

        The old check was literally ``marker in completed.stdout``. This pins
        that a string which SATISFIED that predicate does not satisfy the
        current one -- so a future edit back to a substring test fails here.
        """
        output = "NOT_CAP_X_PASSED_YET waiting for the real gate\n"
        self.assertIn("CAP_X_PASS", output)  # the predicate that used to certify
        found, _ = RUNNER.terminal_marker_ok(output, "CAP_X_PASS")
        self.assertFalse(found)

    def test_exact_marker_line_is_accepted(self) -> None:
        found, problems = RUNNER.terminal_marker_ok("CAP_X_PASS\n", "CAP_X_PASS")
        self.assertTrue(found, problems)
        self.assertEqual(problems, [])

    def test_marker_with_trailing_fields_is_accepted(self) -> None:
        found, problems = RUNNER.terminal_marker_ok(
            "CAP_X_PASS checks=7 cases=3\n", "CAP_X_PASS"
        )
        self.assertTrue(found, problems)

    def test_suffixed_marker_is_refused(self) -> None:
        found, problems = RUNNER.terminal_marker_ok("CAP_X_PASSED\n", "CAP_X_PASS")
        self.assertFalse(found)
        self.assertTrue(any("not present" in p for p in problems), problems)

    def test_prefixed_marker_is_refused(self) -> None:
        found, _ = RUNNER.terminal_marker_ok("NOT_CAP_X_PASS\n", "CAP_X_PASS")
        self.assertFalse(found)

    def test_marker_mentioned_mid_line_is_refused(self) -> None:
        found, _ = RUNNER.terminal_marker_ok(
            "we hope to reach CAP_X_PASS one day\n", "CAP_X_PASS"
        )
        self.assertFalse(found)

    def test_indented_marker_is_refused(self) -> None:
        found, _ = RUNNER.terminal_marker_ok("    CAP_X_PASS\n", "CAP_X_PASS")
        self.assertFalse(found)

    def test_duplicate_marker_lines_are_refused(self) -> None:
        found, problems = RUNNER.terminal_marker_ok(
            "CAP_X_PASS\nCAP_X_PASS\n", "CAP_X_PASS"
        )
        self.assertFalse(found)
        self.assertTrue(any("2 times" in p for p in problems), problems)

    def test_conflicting_terminal_verdict_is_refused(self) -> None:
        found, problems = RUNNER.terminal_marker_ok(
            "CAP_X_PASS\nCAP_X_FAIL\n", "CAP_X_PASS"
        )
        self.assertFalse(found)
        self.assertTrue(any("CAP_X_FAIL" in p for p in problems), problems)

    def test_conflicting_withheld_verdict_is_refused(self) -> None:
        found, problems = RUNNER.terminal_marker_ok(
            "CAP_X_PASS\nCAP_X_WITHHELD reason=no_data\n", "CAP_X_PASS"
        )
        self.assertFalse(found)
        self.assertTrue(any("CAP_X_WITHHELD" in p for p in problems), problems)

    def test_non_terminal_marker_has_no_conflict_set(self) -> None:
        # A fixture's expected_markers need not end in a terminal word; those
        # are progress assertions, and demanding a verdict family of them would
        # be inventing a rule the manifest never made.
        found, problems = RUNNER.terminal_marker_ok(
            "HELDOUT_STAGE_TWO ok\n", "HELDOUT_STAGE_TWO"
        )
        self.assertTrue(found, problems)

    # A fixture's expected_markers are field probes meant to match inside a
    # line ("acc_on=", "semantic=2"), so they stay substring checks. Anything
    # SHAPED like a verdict must not get in through that weaker path.

    def test_field_probes_are_not_treated_as_verdicts(self) -> None:
        for probe in ("acc_on=", "semantic=2", "authority=cnet", "Passed"):
            self.assertFalse(RUNNER.looks_terminal(probe), probe)

    def test_verdict_shaped_expected_markers_are_treated_as_verdicts(self) -> None:
        for verdict in (
            "CAP_X_PASS", "CAP_X_FAIL", "CAP_X_WITHHELD",
            "CAP_X_BLOCKED", "CAP_X_NO_VERDICT", "CAP_X_AMBIGUOUS",
        ):
            self.assertTrue(RUNNER.looks_terminal(verdict), verdict)

    # --- metric sourcing --------------------------------------------------

    def test_metric_comes_from_the_receipt_not_a_default(self) -> None:
        _, _, detail = RUNNER.check_receipt(
            receipt(cases=2, consumed=2, passed=1, case_ids=("a", "b")),
            "test_capability",
            FIXTURE_SHA,
            ["a", "b"],
        )
        metric = RUNNER.extract_metric(
            {"metric_source": "heldout_receipt"}, "", detail
        )
        self.assertEqual(metric, 0.5)

    def test_receipt_metric_without_a_receipt_is_an_error(self) -> None:
        with self.assertRaisesRegex(ValueError, "no receipt metric"):
            RUNNER.extract_metric({"metric_source": "heldout_receipt"}, "", {})


if __name__ == "__main__":
    result = unittest.main(exit=False)
    if result.result.wasSuccessful():
        print(
            "CAPABILITY_CERT_RUNNER_PASS shell=disabled receipt=required "
            f"checks={result.result.testsRun}"
        )
    raise SystemExit(0 if result.result.wasSuccessful() else 1)
