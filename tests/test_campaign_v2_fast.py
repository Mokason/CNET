#!/usr/bin/env python3
"""Hermetic gate for campaign v2-fast prepare/export/estimate path."""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANALYZE = ROOT / "tools" / "margin_sweep_analyze.py"
DRIVER = ROOT / "tools" / "campaign_v2_fast.sh"
REAL_SWEEP = ROOT / "qwythos_english_v1.margin_sweep.tsv"


def _tiny_sweep(path: Path, n_units: int = 8, probes: int = 32) -> None:
    """Write a miniature sweep: half units minable under set@0.02."""
    lines = ["# margin_sweep synthetic V=%d units=%d\n" % (probes, n_units)]
    lines.append("unit_idx unit_token w_idx w_token ordered_margin set_margin\n")
    for u in range(n_units):
        tok = 1000 + u
        for w in range(probes):
            # first half: high set margins; second half: near-zero set margins
            if u < n_units // 2:
                om, sm = 0.05, 0.10
            else:
                om, sm = 0.001, 0.001
            lines.append("%d %d %d %d %.6f %.6f\n" % (u, tok, w, 2000 + w, om, sm))
    path.write_text("".join(lines), encoding="utf-8")


class CampaignV2FastTest(unittest.TestCase):
    def test_export_minable_set_semantics(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            sweep = td_path / "s.tsv"
            allow = td_path / "minable.ids"
            _tiny_sweep(sweep, n_units=8, probes=32)
            proc = subprocess.run(
                [
                    sys.executable,
                    str(ANALYZE),
                    str(sweep),
                    "--eps",
                    "0.02",
                    "--semantics",
                    "set",
                    "--export-minable",
                    str(allow),
                ],
                check=False,
                capture_output=True,
                text=True,
                cwd=str(ROOT),
            )
            self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
            toks = [
                int(x)
                for x in allow.read_text(encoding="utf-8").splitlines()
                if x.strip() and not x.startswith("#")
            ]
            self.assertEqual(len(toks), 4, "half of synthetic units minable")
            self.assertEqual(toks, [1000, 1001, 1002, 1003])

    def test_driver_prepare_and_estimate(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            sweep = td_path / "s.tsv"
            allow = td_path / "minable.ids"
            _tiny_sweep(sweep)
            prep = subprocess.run(
                [
                    "bash",
                    str(DRIVER),
                    "prepare",
                    "--sweep",
                    str(sweep),
                    "--out-allowlist",
                    str(allow),
                    "--eps",
                    "0.02",
                    "--semantics",
                    "set",
                ],
                check=False,
                capture_output=True,
                text=True,
                cwd=str(ROOT),
            )
            self.assertEqual(prep.returncode, 0, prep.stderr + prep.stdout)
            self.assertIn("CAMPAIGN_V2_FAST_PREPARE_PASS", prep.stdout)
            est = subprocess.run(
                [
                    "bash",
                    str(DRIVER),
                    "estimate",
                    "--allowlist",
                    str(allow),
                    "--sec-per-unit",
                    "70",
                    "--workers",
                    "1",
                ],
                check=False,
                capture_output=True,
                text=True,
                cwd=str(ROOT),
            )
            self.assertEqual(est.returncode, 0, est.stderr + est.stdout)
            self.assertIn("CAMPAIGN_V2_FAST_ESTIMATE", est.stdout)
            self.assertIn("minable_units:     4", est.stdout)

    def test_real_sweep_export_if_present(self) -> None:
        if not REAL_SWEEP.is_file():
            self.skipTest("real margin sweep TSV not in tree")
        with tempfile.TemporaryDirectory() as td:
            allow = Path(td) / "real.ids"
            proc = subprocess.run(
                [
                    sys.executable,
                    str(ANALYZE),
                    str(REAL_SWEEP),
                    "--eps",
                    "0.02",
                    "--semantics",
                    "set",
                    "--export-minable",
                    str(allow),
                ],
                check=False,
                capture_output=True,
                text=True,
                cwd=str(ROOT),
            )
            self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
            n = sum(
                1
                for x in allow.read_text(encoding="utf-8").splitlines()
                if x.strip() and not x.startswith("#")
            )
            # Measured verdict: 255/256 set-minable at eps 0.02
            self.assertGreaterEqual(n, 250)
            self.assertLessEqual(n, 256)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if result.wasSuccessful():
        print("CAMPAIGN_V2_FAST_PASS")
        sys.exit(0)
    sys.exit(1)
