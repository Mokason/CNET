#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import math
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "run_real_model_acceptance.py"
spec = importlib.util.spec_from_file_location("run_real_model_acceptance", MODULE_PATH)
assert spec and spec.loader
campaign = importlib.util.module_from_spec(spec)
spec.loader.exec_module(campaign)


class RealModelAcceptanceTests(unittest.TestCase):
    def test_gguf_and_qgkp_magic_are_enforced(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            gguf = root / "model.gguf"
            qgkp = root / "model.qgkp"
            bad = root / "bad.bin"
            gguf.write_bytes(b"GGUFpayload")
            qgkp.write_bytes(b"QGKPpayload")
            bad.write_bytes(b"NOPEpayload")
            campaign.require_magic(gguf, b"GGUF")
            campaign.require_magic(qgkp, b"QGKP")
            with self.assertRaises(ValueError):
                campaign.require_magic(bad, b"GGUF")

    def test_top_logprob_entropy_is_normalized(self) -> None:
        peaked = [{"id": 1, "logprob": math.log(0.99)}, {"id": 2, "logprob": math.log(0.01)}]
        flat = [{"id": 1, "logprob": math.log(0.5)}, {"id": 2, "logprob": math.log(0.5)}]
        self.assertLess(campaign.normalized_entropy(peaked), 0.1)
        self.assertGreater(campaign.normalized_entropy(flat), 0.99)

    def test_distribution_metrics_and_recovery_curve_use_real_token_ids(self) -> None:
        reference = [{"id": 7, "logprob": math.log(0.8)}, {"id": 9, "logprob": math.log(0.2)}]
        candidate = [{"id": 7, "logprob": math.log(0.5)}, {"id": 11, "logprob": math.log(0.5)}]
        metrics = campaign.distribution_metrics(reference, candidate)
        self.assertGreater(metrics["residual_l2_norm"], 0.0)
        curve = metrics["recovery_curve"]
        self.assertEqual([point["alpha"] for point in curve], [0.0, 0.25, 0.5, 0.75, 1.0])
        self.assertGreater(curve[0]["residual_l2_norm"], curve[-1]["residual_l2_norm"])
        self.assertAlmostEqual(curve[-1]["residual_l2_norm"], 0.0, places=7)

    def test_candidate_is_quarantined_on_quality_regression(self) -> None:
        reference = {"quality_pass_fraction": 1.0}
        candidate = {"quality_pass_fraction": 0.0}
        admission = campaign.admit_candidate(reference, candidate, max_quality_delta=0.0)
        self.assertFalse(admission["admitted"])
        self.assertEqual(admission["selected_role"], "reference")
        self.assertIn("quality_regression", admission["reasons"])

    def test_candidate_can_be_admitted_without_regression(self) -> None:
        reference = {"quality_pass_fraction": 2 / 3}
        candidate = {"quality_pass_fraction": 2 / 3}
        admission = campaign.admit_candidate(reference, candidate, max_quality_delta=0.0)
        self.assertTrue(admission["admitted"])
        self.assertEqual(admission["selected_role"], "candidate")


if __name__ == "__main__":
    unittest.main()
