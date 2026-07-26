#!/usr/bin/env python3

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


class CapabilityCertRunnerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="cnet-cert-test-")
        self.root = Path(self.temporary.name).resolve()
        (self.root / "fixtures").mkdir()
        self.fixture = self.root / "fixtures" / "held_out.json"
        self.fixture.write_text(
            json.dumps(
                {
                    "capability_id": "test_capability",
                    "cases": [{"input": "unseen", "expected": "pass"}],
                    "expected_markers": ["TEST_PASS"],
                }
            ),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def manifest(self, evaluator: list[str] | None = None) -> Path:
        path = self.root / "manifest.json"
        path.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "capability_id": "test_capability",
                    "held_out_fixture": "fixtures/held_out.json",
                    "evaluator": evaluator or ["make", "test_capability"],
                    "required_marker": "TEST_PASS",
                    "title": "Test capability",
                    "owner": "test",
                    "evidence_artifact": "logs/test_capability.log",
                    "failure_envelope": "Fails if held-out behavior regresses.",
                    "absolute_floor": 1.0,
                    "baseline_metric": 1.0,
                    "regression_budget": 0.0,
                }
            ),
            encoding="utf-8",
        )
        return path

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
                self.root, self.manifest(["sh", "-c", "touch /tmp/pwned"])
            )

    def test_fixture_path_cannot_escape_repository(self) -> None:
        path = self.manifest()
        value = json.loads(path.read_text(encoding="utf-8"))
        value["held_out_fixture"] = "../outside.json"
        path.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "escapes repository"):
            RUNNER.validate_manifest(self.root, path)


if __name__ == "__main__":
    result = unittest.main(exit=False)
    if result.result.wasSuccessful():
        print("CAPABILITY_CERT_RUNNER_PASS metric=1.000 shell=disabled")
    raise SystemExit(0 if result.result.wasSuccessful() else 1)
