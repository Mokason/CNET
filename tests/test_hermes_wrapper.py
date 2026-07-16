#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "render_hermes_wrapper.py"
spec = importlib.util.spec_from_file_location("render_hermes_wrapper", MODULE_PATH)
assert spec and spec.loader
wrapper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wrapper)


class HermesWrapperTests(unittest.TestCase):
    def sample_report(self, root: Path) -> dict:
        selected = root / "selected.gguf"
        selected.write_bytes(b"GGUFmodel")
        return {
            "schema_version": 1,
            "overall_pass": True,
            "verdict": "PASS_CANDIDATE_QUARANTINED",
            "artifacts": {
                "reference": {"path": str(selected), "sha256": "abc", "bytes": selected.stat().st_size},
                "candidate": {"path": str(root / "bad.gguf"), "sha256": "def", "bytes": 5},
                "qgkp": {"path": str(root / "source.qgkp"), "round_trip": {"byte_identical": True}},
            },
            "admission": {"admitted": False, "selected_role": "reference", "reasons": ["quality_regression"]},
            "selected_model": {"role": "reference", "path": str(selected)},
            "restart_integrity": {"responses_identical": True, "quality_preserved": True},
        }

    def test_manifest_selects_only_admitted_or_fallback_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            report = self.sample_report(root)
            manifest = wrapper.build_manifest(report, Path("report.json"), Path("/bin/llama-server"))
            self.assertEqual(manifest["selected_model"]["role"], "reference")
            self.assertEqual(manifest["selected_model"]["path"], report["selected_model"]["path"])
            self.assertTrue(manifest["runtime"]["cpu_only"])
            self.assertGreaterEqual(manifest["runtime"]["ctx_size"], 65_536)
            self.assertEqual(manifest["hermes"]["provider"], "custom")
            self.assertEqual(manifest["hermes"]["probe_expected"], "Ready")
            self.assertIn("qwen3.5", manifest["hermes"]["model"].lower())
            self.assertLessEqual(manifest["hermes"]["max_output_tokens"], 256)
            self.assertGreaterEqual(manifest["hermes"]["query_timeout_seconds"], 180)
            self.assertLessEqual(manifest["hermes"]["query_timeout_seconds"], 600)
            self.assertIn("run_hermes_wrapper.py", manifest["hermes"]["start_command"])

    def test_failed_campaign_cannot_produce_wrapper(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            report = self.sample_report(Path(td))
            report["overall_pass"] = False
            with self.assertRaises(ValueError):
                wrapper.build_manifest(report, Path("report.json"), Path("/bin/llama-server"))

    def test_manifest_round_trips_as_json(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            manifest = wrapper.build_manifest(
                self.sample_report(root), Path("report.json"), Path("/bin/llama-server")
            )
            self.assertEqual(json.loads(json.dumps(manifest))["schema_version"], 1)


if __name__ == "__main__":
    unittest.main()
