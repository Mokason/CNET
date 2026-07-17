#!/usr/bin/env python3
"""Deploy profile static gate: free-win knobs present, quality floors intact."""
from pathlib import Path
import re
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / "config" / "cnet-deploy.env"
SERVICE = ROOT / "config" / "cnet-gap-lane.service"
V2 = ROOT / "config" / "qwythos_v2_campaign.env"
APPLY = ROOT / "scripts" / "apply_deploy_profile.sh"


class DeployProfileTest(unittest.TestCase):
    def test_profile_has_free_wins(self) -> None:
        text = PROFILE.read_text(encoding="utf-8")
        for key in (
            "CNET_ORACLE_INT8=1",
            "CNET_TRAIN_FAST=1",
            "CNET_ACQ_STAGES=40",
            "CNET_LANE_MAX_CLOSURES=4",
            "CNET_TEACHER_IDLE_SEC=300",
            "CNET_HEALTH_TICK_SECONDS=300",
        ):
            self.assertIn(key, text, f"missing deploy free-win {key}")
        # Must NOT silently enable TOPK_SET (new goldens required).
        self.assertNotRegex(
            text,
            r"^export CNET_TOPK_SET=1",
            "TOPK_SET must not be silent deploy default",
        )

    def test_v2_campaign_is_explicit(self) -> None:
        text = V2.read_text(encoding="utf-8")
        self.assertIn("CNET_TOPK_SET=1", text)
        self.assertIn("NEW goldens", text)

    def test_gap_lane_service_has_budget_knobs(self) -> None:
        text = SERVICE.read_text(encoding="utf-8")
        self.assertIn("CNET_ORACLE_INT8=1", text)
        self.assertIn("CNET_TRAIN_FAST", text)
        self.assertIn("CNET_ACQ_STAGES", text)
        self.assertIn("CNET_LANE_MAX_CLOSURES", text)
        self.assertIn("CNET_TEACHER_IDLE_SEC", text)

    def test_apply_script_prints_ok(self) -> None:
        proc = subprocess.run(
            ["bash", str(APPLY)],
            check=False,
            capture_output=True,
            text=True,
            cwd=str(ROOT),
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("CNET_DEPLOY_PROFILE_OK", proc.stdout)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if result.wasSuccessful():
        print("DEPLOY_PROFILE_PASS")
        sys.exit(0)
    sys.exit(1)
