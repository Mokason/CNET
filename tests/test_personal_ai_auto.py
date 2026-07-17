#!/usr/bin/env python3
"""Static gate for Personal AI automation units + script."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "personal_ai_auto.sh"
LANE = ROOT / "config" / "cnet-personal-ai-lane.service"
TARGET = ROOT / "config" / "cnet-personal-ai.target"
ENV = ROOT / "config" / "personal-ai.env"


class PersonalAiAutoTest(unittest.TestCase):
    def test_lane_unit_has_failsafe_and_exec(self) -> None:
        text = LANE.read_text(encoding="utf-8")
        self.assertIn("ConditionFileIsExecutable=", text)
        self.assertIn("gap_lane_run", text)
        self.assertIn("CNET_GAP_INBOX", text)
        self.assertIn("CNET_TRAIN_FAST", text)
        self.assertIn("CNET_TEACHER_IDLE_SEC", text)
        # Condition binary matches ExecStart gap_lane_run path fragment
        cond = re.search(r"ConditionFileIsExecutable=(\S+)", text)
        self.assertIsNotNone(cond)
        self.assertTrue(cond.group(1).endswith("gap_lane_run"))

    def test_personal_env_local_first(self) -> None:
        text = ENV.read_text(encoding="utf-8")
        self.assertIn("CNET_PERSONAL_ALLOW_TEACHER=1", text)
        self.assertIn("CNET_PERSONAL_TEACH_INLINE=0", text)
        self.assertIn("CNET_TEACHER_IDLE_SEC", text)
        self.assertIn("CNET_RESIDUAL_GGUF=", text)
        self.assertNotIn("# CNET_RESIDUAL_GGUF=", text)
        self.assertIn("CNET_RESIDUAL_WINDOW=", text)

    def test_script_help(self) -> None:
        proc = subprocess.run(
            ["bash", str(SCRIPT)],
            check=False,
            capture_output=True,
            text=True,
            cwd=str(ROOT),
        )
        self.assertEqual(proc.returncode, 2)
        out = proc.stdout + proc.stderr
        self.assertIn("prepare|install|start", out)
        self.assertIn("serve-proof", out)
        self.assertIn("loop", out)
        self.assertIn("jtc-seal", out)
        self.assertIn("ops-install", out)
        self.assertIn("ops-tick", out)

    def test_prepare_dry_structure(self) -> None:
        # prepare needs make; may be heavy — only check script parses + target exists
        self.assertTrue(TARGET.is_file())
        self.assertTrue(SCRIPT.is_file())
        self.assertIn("PERSONAL_AI_AUTO_PREPARE_OK", SCRIPT.read_text(encoding="utf-8"))
        common = ROOT / "scripts" / "personal_ai_common.sh"
        self.assertTrue(common.is_file())
        text = common.read_text(encoding="utf-8")
        self.assertIn("cnet_count_lines", text)
        self.assertIn("cnet_default_base", text)

    def test_metrics_json_empty_inbox(self) -> None:
        """grep -c || echo 0 must not emit double-zero JSON (empty file)."""
        with tempfile.TemporaryDirectory() as td:
            base = Path(td) / "fake.cnb"
            base.write_bytes(b"x")
            inbox = Path(str(base) + ".inbox")
            inbox.write_text("")  # empty → zero matches
            proc = subprocess.run(
                ["bash", str(ROOT / "scripts" / "personal_ai_metrics.sh"), str(base)],
                check=False,
                capture_output=True,
                text=True,
                cwd=str(ROOT),
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            import json

            data = json.loads(proc.stdout)
            self.assertEqual(data["inbox_lines"], 0)
            self.assertEqual(data["inbox_no_plan"], 0)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if result.wasSuccessful():
        print("PERSONAL_AI_AUTO_PASS")
        sys.exit(0)
    sys.exit(1)
