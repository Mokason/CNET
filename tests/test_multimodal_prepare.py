#!/usr/bin/env python3
"""Static gate for multimodal campaign helper + plan."""
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "multimodal_campaign.sh"
PLAN = ROOT / "plans" / "multimodal_external_teachers.md"


class MultimodalPrepareTest(unittest.TestCase):
    def test_plan_exists(self) -> None:
        text = PLAN.read_text(encoding="utf-8")
        self.assertIn("External Teachers", text)
        self.assertIn("Voice v0", text)
        self.assertIn("Vision v0", text)

    def test_prepare(self) -> None:
        proc = subprocess.run(
            ["bash", str(SCRIPT), "prepare"],
            check=False,
            capture_output=True,
            text=True,
            cwd=str(ROOT),
        )
        self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
        self.assertIn("MULTIMODAL_PREPARE_PASS", proc.stdout)
        self.assertTrue((ROOT / "artifacts" / "voice_v0_commands.txt").is_file())
        self.assertTrue((ROOT / "artifacts" / "vision_v0_classes.txt").is_file())


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if result.wasSuccessful():
        print("MULTIMODAL_PREPARE_PASS")
        sys.exit(0)
    sys.exit(1)
