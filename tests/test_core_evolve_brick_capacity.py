"""BRICK_EVOLVE_RED: an invalid inventory must never look like an empty bank."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class EvolveCapacityTests(unittest.TestCase):
    def run_bank(self, count, malformed=False):
        binary = Path(os.environ.get("CNET_EVOLVE_BIN", "bin/cnet_core_evolve")).absolute()
        with tempfile.TemporaryDirectory(prefix="cnet-evolve-capacity-") as directory:
            root = Path(directory)
            direction = root / "evolve_direction.conf"
            direction.write_text("allow_live_miss=0\nallow_factory=0\nallow_goals=0\n"
                                 "allow_obsidian=0\nallow_agi_tick=0\n")
            for index in range(count):
                (root / f"fixture_{index}.lut").write_text(
                    f"tag=fixture_{index}\nlut=" + ",".join(str(k) for k in range(16)) + "\n")
            if malformed:
                (root / "bad.lut").write_text("tag=bad\nlut=nan\n")
            before = {p.name: p.read_bytes() for p in root.iterdir()}
            env = {"PATH": "/usr/bin:/bin", "LC_ALL": "C", "CNET_CORE_BUS_BRICKS_DIR": str(root),
                   "CNET_EVOLVE_DIRECTION": str(direction), "CNET_GROK_GUIDE": "0",
                   "CNET_BONSAI_GGUF": str(root / "no-model.gguf")}
            result = subprocess.run([str(binary), "--once"], cwd=root, env=env,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(before, {p.name: p.read_bytes() for p in root.iterdir()},
                             "inspection tick must not change the fixture bank")
            return result

    def test_counts_expanded_inventory(self):
        for count in (25, 256):
            with self.subTest(count=count):
                result = self.run_bank(count)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"luts_before={count} luts_after={count}", result.stdout)

    def test_refuses_overflow_without_success_marker(self):
        result = self.run_bank(257)
        self.assertNotEqual(result.returncode, 0, "BRICK_EVOLVE_RED overflow reported success")
        self.assertIn("BRICK_BANK_INVALID stage=before", result.stderr)
        self.assertNotIn("CNET_CORE_EVOLVE_OK", result.stdout)

    def test_refuses_malformed_without_success_marker(self):
        result = self.run_bank(1, malformed=True)
        self.assertNotEqual(result.returncode, 0, "BRICK_EVOLVE_RED malformed reported success")
        self.assertIn("BRICK_BANK_INVALID stage=before", result.stderr)
        self.assertNotIn("CNET_CORE_EVOLVE_OK", result.stdout)


if __name__ == "__main__":
    unittest.main()
