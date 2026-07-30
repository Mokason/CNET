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
from pathlib import Path
import tempfile
import unittest


RUNNER_PATH = Path(__file__).with_name("run_capability_cert.py")
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
