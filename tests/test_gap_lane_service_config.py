#!/usr/bin/env python3
"""Fail-safe deployment tracer for the CNET gap-lane systemd unit.

RED→GREEN contract:
  1. The unit must declare ``ConditionFileIsExecutable=<bin>`` in the
     ``[Unit]`` section, where ``<bin>`` is the executable at the start of
     ``ExecStart``.  When the daemon binary is absent the condition skips the
     unit cleanly instead of triggering 203/EXEC restart churn.
  2. The test statically validates the tracked unit and, when available,
     asks ``systemd-analyze condition`` to prove the directive accepts a real
     executable and rejects a missing one. It never starts or enables a unit.
     Gemma stays offline.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
SERVICE = ROOT / "config" / "cnet-gap-lane.service"


def _parse_unit(text: str) -> tuple[str, str]:
    """Return (unit-section-text, execstart-binary)."""
    unit_match = re.search(r"^\[Unit\]\s*\n(.*?)(?=^\[)", text, re.MULTILINE | re.DOTALL)
    if not unit_match:
        return "", ""
    unit_body = unit_match.group(1)

    exec_match = re.search(r"^ExecStart=(\S+)", text, re.MULTILINE)
    exec_bin = exec_match.group(1) if exec_match else ""
    return unit_body, exec_bin


class GapLaneServiceConfigTest(unittest.TestCase):
    def test_condition_matches_execstart_binary(self) -> None:
        text = SERVICE.read_text(encoding="utf-8")
        unit_body, exec_bin = _parse_unit(text)

        self.assertTrue(exec_bin, "ExecStart is missing or empty")
        self.assertTrue(unit_body, "[Unit] section not found")

        all_conditions = re.findall(
            r"^(!?)ConditionFileIsExecutable=(\S+)", text, re.MULTILINE
        )
        self.assertEqual(
            1, len(all_conditions),
            "unit must declare exactly one ConditionFileIsExecutable",
        )
        self.assertEqual("", all_conditions[0][0], "condition must not be negated")

        # The condition must name exactly the ExecStart binary and live in
        # [Unit], never [Service] or [Install].
        cond_match = re.search(
            r"^ConditionFileIsExecutable=(\S+)", unit_body, re.MULTILINE
        )
        self.assertIsNotNone(
            cond_match,
            "[Unit] lacks ConditionFileIsExecutable — absent binary will "
            "cause 203/EXEC restart churn instead of a clean condition skip",
        )
        if cond_match is None:  # narrows Optional[Match] for type checkers
            return
        self.assertEqual(
            cond_match.group(1),
            exec_bin,
            f"ConditionFileIsExecutable ({cond_match.group(1)}) does not "
            f"match ExecStart binary ({exec_bin})",
        )

    def test_systemd_condition_semantics_when_available(self) -> None:
        analyze = shutil.which("systemd-analyze")
        if analyze is None:
            self.skipTest("systemd-analyze is unavailable on this host")

        present = subprocess.run(
            [analyze, "condition", "ConditionFileIsExecutable=/bin/sh"],
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(0, present.returncode, present.stderr or present.stdout)

        missing_path = ROOT / "bin" / ".cnet-condition-probe-missing"
        self.assertFalse(missing_path.exists(), "semantic probe path unexpectedly exists")
        missing = subprocess.run(
            [analyze, "condition", f"ConditionFileIsExecutable={missing_path}"],
            check=False, capture_output=True, text=True,
        )
        self.assertNotEqual(
            0, missing.returncode,
            "ConditionFileIsExecutable must fail for a missing path",
        )


if __name__ == "__main__":
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.TestLoader().loadTestsFromName("__main__")
    )
    if result.wasSuccessful():
        print("GAP_LANE_SERVICE_CONFIG_PASS")
    else:
        print("GAP_LANE_SERVICE_CONFIG_FAIL", file=sys.stderr)
    raise SystemExit(0 if result.wasSuccessful() else 1)