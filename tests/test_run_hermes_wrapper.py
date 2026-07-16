#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "run_hermes_wrapper.py"
spec = importlib.util.spec_from_file_location("run_hermes_wrapper", MODULE_PATH)
assert spec and spec.loader
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RunHermesWrapperTests(unittest.TestCase):
    def manifest(self, root: Path) -> dict:
        model = root / "selected.gguf"
        model.write_bytes(b"GGUFmodel")
        digest = hashlib.sha256(model.read_bytes()).hexdigest()
        return {
            "schema_version": 1,
            "kind": "cnet-hermes-wrapper",
            "selected_model": {"role": "reference", "path": str(model), "sha256": digest},
            "runtime": {
                "backend": "llama-server",
                "server": "/bin/llama-server",
                "cpu_only": True,
                "host": "127.0.0.1",
                "port": 0,
                "threads": 4,
                "ctx_size": 65536,
                "parallel": 1,
                "gpu_layers": 0,
            },
            "hermes": {
                "provider": "custom",
                "model": "qwen/qwen3.5-test",
                "default_probe": "Reply with exactly Ready",
                "probe_expected": "Ready",
                "max_output_tokens": 64,
                "max_turns": 2,
                "query_timeout_seconds": 540,
            },
        }

    def test_manifest_validation_is_hash_and_cpu_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            manifest = self.manifest(Path(td))
            selected = runner.validate_manifest(manifest)
            self.assertEqual(selected.name, "selected.gguf")
            manifest["runtime"]["gpu_layers"] = 1
            with self.assertRaises(ValueError):
                runner.validate_manifest(manifest)
            manifest["runtime"]["gpu_layers"] = 0
            manifest["runtime"]["ctx_size"] = 8192
            with self.assertRaises(ValueError):
                runner.validate_manifest(manifest)

    def test_hash_mismatch_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            manifest = self.manifest(Path(td))
            manifest["selected_model"]["sha256"] = "0" * 64
            with self.assertRaises(ValueError):
                runner.validate_manifest(manifest)

    def test_commands_pin_cpu_and_custom_provider(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            manifest = self.manifest(Path(td))
            model = runner.validate_manifest(manifest)
            server = runner.build_server_command(manifest, model, 18888)
            self.assertIn("--n-gpu-layers", server)
            self.assertEqual(server[server.index("--n-gpu-layers") + 1], "0")
            self.assertIn("--jinja", server)
            self.assertIn("--chat-template-kwargs", server)
            self.assertEqual(server[server.index("--chat-template-kwargs") + 1], '{"enable_thinking":false}')
            hermes = runner.build_hermes_command(manifest, "probe")
            self.assertIn("--provider", hermes)
            self.assertEqual(hermes[hermes.index("--provider") + 1], "custom")
            self.assertEqual(hermes[hermes.index("--max-turns") + 1], "2")
            self.assertIn("--ignore-rules", hermes)
            self.assertNotIn("--ignore-user-config", hermes)
            config = runner.build_hermes_config(manifest, "http://127.0.0.1:18888/v1")
            self.assertEqual(config["model"]["max_tokens"], 64)
            self.assertNotIn("max_tokens", config)
            self.assertEqual(config["model"]["context_length"], 65536)
            self.assertEqual(config["model"]["base_url"], "http://127.0.0.1:18888/v1")

    def test_probe_match_requires_clean_final_line(self) -> None:
        self.assertTrue(runner.probe_matched("warning\nReady", "Ready"))
        self.assertFalse(runner.probe_matched("Reasoning mentions Ready\nnot final", "Ready"))
        self.assertFalse(runner.probe_matched("Ready\nReached maximum iterations (2)", "Ready"))


if __name__ == "__main__":
    unittest.main()
