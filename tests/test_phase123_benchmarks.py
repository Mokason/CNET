#!/usr/bin/env python3
import importlib.util
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "run_phase123_benchmarks.py"
spec = importlib.util.spec_from_file_location("phase123", MODULE_PATH)
assert spec is not None and spec.loader is not None
phase123 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(phase123)


class Phase123BenchmarkAuthorityTests(unittest.TestCase):
    def test_truthfulqa_claim_is_withheld_without_dataset_and_runtime(self) -> None:
        result = phase123.truthfulqa_claim(
            contract_pass=True,
            dataset_present=False,
            runtime_integrated=False,
            measured_gain=None,
        )
        self.assertEqual(result["status"], "withheld")
        self.assertFalse(result["success_metric_claimed"])
        self.assertEqual(result["target_gain"], 0.025)

    def test_truthfulqa_claim_requires_both_prerequisites(self) -> None:
        with self.assertRaises(ValueError):
            phase123.truthfulqa_claim(
                contract_pass=True,
                dataset_present=False,
                runtime_integrated=True,
                measured_gain=0.04,
            )

    def test_truthfulqa_measured_gain_uses_declared_floor(self) -> None:
        passed = phase123.truthfulqa_claim(True, True, True, 0.03)
        failed = phase123.truthfulqa_claim(True, True, True, 0.02)
        self.assertEqual(passed["status"], "measured_pass")
        self.assertEqual(failed["status"], "measured_fail")

    def test_long_context_claim_is_withheld_without_execution_integration(self) -> None:
        result = phase123.long_context_claim(
            contract_pass=True,
            dataset_present=False,
            runtime_integrated=False,
            budget_measurements=[{"budget_fraction": 0.20, "needle_retention": 1.0}],
        )
        self.assertEqual(result["status"], "withheld")
        self.assertFalse(result["quality_metric_claimed"])
        self.assertEqual(result["local_selector_measurements"][0]["needle_retention"], 1.0)

    def test_creative_preservation_accepts_only_bounded_non_regression(self) -> None:
        passed = phase123.creative_preservation([0.80, 0.70], [0.78, 0.72], tolerance=0.05)
        failed = phase123.creative_preservation([0.80, 0.70], [0.60, 0.62], tolerance=0.05)
        self.assertTrue(passed["passed"])
        self.assertFalse(failed["passed"])
        self.assertAlmostEqual(passed["delta"], 0.0)

    def test_overall_verdict_names_withheld_external_claims(self) -> None:
        result = phase123.overall_verdict(
            {"contract_pass": True, "status": "withheld"},
            {"contract_pass": True, "status": "withheld"},
            {"contract_pass": True, "status": "measured_pass"},
        )
        self.assertEqual(result, "PASS_WITH_EXTERNAL_CLAIMS_WITHHELD")


if __name__ == "__main__":
    unittest.main()
