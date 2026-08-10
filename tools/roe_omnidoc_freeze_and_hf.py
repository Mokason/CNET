#!/usr/bin/env python3
"""Freeze OmniDoc full report + build HF package when preds complete.

Usage:
  .venv-unlimited-ocr/bin/python tools/roe_omnidoc_freeze_and_hf.py           # check + freeze if ready
  .venv-unlimited-ocr/bin/python tools/roe_omnidoc_freeze_and_hf.py --eval   # run official eval first
  .venv-unlimited-ocr/bin/python tools/roe_omnidoc_freeze_and_hf.py --publish  # hf upload if gated
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PRED = ROOT / "artifacts/omnidoc_full/predictions_unlimited"
REPORT = ROOT / "artifacts/omnidoc_full/OFFICIAL_FULL_CDM_REPORT.json"
DONE = ROOT / "artifacts/omnidoc_full/OMNIDOC_FULL_JOB_DONE.txt"
HF = ROOT / "artifacts/omnidoc_full/hf_package"
METRIC = ROOT / "result/predictions_unlimited_quick_match_metric_result.json"
CFG = ROOT / "artifacts/omnidoc_full/end2end_unlimited_full_cdm.yaml"
TARGET = 1645


def n_preds() -> int:
    return len(list(PRED.glob("*.md"))) if PRED.is_dir() else 0


def run_eval() -> int:
    env = os.environ.copy()
    env["PATH"] = "/usr/local/bin:" + env.get("PATH", "")
    env["PYTHONPATH"] = str(ROOT / "third_party/OmniDocBench")
    env["CDM_SAVE_VIS"] = "0"
    log = ROOT / "logs/roe_omnidoc_full_cdm_eval.log"
    print(f"[freeze] official eval → {log}", flush=True)
    with open(log, "w") as f:
        p = subprocess.run(
            [
                str(ROOT / ".venv-unlimited-ocr/bin/python"),
                str(ROOT / "third_party/OmniDocBench/pdf_validation.py"),
                "--config",
                str(CFG),
            ],
            cwd=str(ROOT),
            env=env,
            stdout=f,
            stderr=subprocess.STDOUT,
        )
    return p.returncode


def parse_metric() -> dict:
    m = json.loads(METRIC.read_text())
    text_ed = m["text_block"]["all"]["Edit_dist"]["ALL_page_avg"]
    cdm = m["display_formula"]["all"]["CDM"]["all"]
    teds = m["table"]["all"]["TEDS"]["all"]
    text_s = (1.0 - text_ed) * 100.0
    teds_s = teds * 100.0
    cdm_s = cdm * 100.0
    overall = (text_s + teds_s + cdm_s) / 3.0
    n = n_preds()
    # claim tier
    if n < TARGET:
        claim = "WITHHELD_INCOMPLETE_PREDS"
        tier = "A"
    elif overall >= 93.0:
        claim = "FULLSET_OVERALL_GE_93_CONFIRMATION"
        tier = "B+"
    else:
        claim = "FULLSET_OVERALL_COMPUTED"
        tier = "B"
    return {
        "n_preds": n,
        "target": 1651,
        "text_Edit_dist": text_ed,
        "text_score": text_s,
        "table_TEDS": teds,
        "table_TEDS_x100": teds_s,
        "formula_CDM": cdm,
        "formula_CDM_x100": cdm_s,
        "Overall": overall,
        "Overall_formula": "((1-text_edit)*100 + TEDS*100 + CDM*100)/3",
        "protocol": "OmniDocBench end2end quick_match + CDM; teds_workers=1 preferred",
        "system": "Unlimited-OCR plain gundam on AMD R9700 ROCm (teacher factory)",
        "roe_role": "asset/runtime not monobrain; A/B vs ROE still separate",
        "claim_tier": tier,
        "sota_claim": claim,
        "second_brain": False,
        "frozen_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "hf_ready": n >= TARGET and METRIC.is_file(),
    }


def build_hf_package(rep: dict) -> None:
    HF.mkdir(parents=True, exist_ok=True)
    (HF / "eval").mkdir(exist_ok=True)
    # copy report + metric + config
    (HF / "eval" / "OFFICIAL_FULL_CDM_REPORT.json").write_text(json.dumps(rep, indent=2))
    if METRIC.is_file():
        shutil.copy2(METRIC, HF / "eval" / "metric_result.json")
    if CFG.is_file():
        shutil.copy2(CFG, HF / "eval" / "end2end_unlimited_full_cdm.yaml")
    # side benches
    for src, name in [
        (ROOT / "artifacts/omnidoc_full/official_cdm40_serial/OFFICIAL_SERIAL_REPORT.json", "smoke40_serial.json"),
        (ROOT / "artifacts/omnidoc_full/side_by_side40/side_by_side_report.json", "side_by_side40.json"),
        (ROOT / "artifacts/omnidoc_full/NEXT_MOVES_RESULTS.md", "NEXT_MOVES_RESULTS.md"),
    ]:
        if src.is_file():
            shutil.copy2(src, HF / "eval" / name)
    # README
    readme_src = HF / "README.md"
    if not readme_src.is_file():
        readme_src = ROOT / "artifacts/omnidoc_full/hf_package/README.md"
    text = readme_src.read_text(encoding="utf-8")
    text = text.replace("{{FULL_REPORT_JSON}}", json.dumps(rep, indent=2))
    # stamp tier
    text = text.replace(
        "**Current tier will be filled from** `eval/OFFICIAL_FULL_CDM_REPORT.json` at publish time.",
        f"**Current claim tier: `{rep['claim_tier']}` — `{rep['sota_claim']}`**",
    )
    if rep.get("hf_ready"):
        text = text.replace("**Status:** DRAFT until full 1651 + CDM report is frozen.", "**Status:** FROZEN full-set report ready for gated publish.")
    (HF / "README.md").write_text(text)
    # publish gate script note
    (HF / "PUBLISH.md").write_text(
        f"""# Publish gate

```bash
# only if hf_ready true and you accept the claim tier
huggingface-cli login
huggingface-cli upload YOUR_ORG/cnet-roe-ocr-omnidoc \\
  /home/marble/AI/CNET/artifacts/omnidoc_full/hf_package . --repo-type model
```

Do **not** upload Unlimited weights here.
Do **not** title the repo \"OmniDoc SOTA\" unless claim_tier is E.

Frozen report:
```json
{json.dumps(rep, indent=2)}
```
"""
    )
    print(f"[freeze] HF package → {HF}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--eval", action="store_true", help="run official eval first")
    ap.add_argument("--force", action="store_true", help="freeze even if preds < target")
    ap.add_argument("--publish", action="store_true", help="hf upload if HF_REPO set and gated")
    args = ap.parse_args()

    n = n_preds()
    print(f"[freeze] preds={n} target>={TARGET}", flush=True)
    if n < TARGET and not args.force:
        print("[freeze] NOT READY — wait for full infer")
        print(f"PROGRESS {n}/1651")
        return 2

    if args.eval or not METRIC.is_file():
        rc = run_eval()
        if rc != 0:
            print(f"[freeze] eval rc={rc}")
            return rc
    if not METRIC.is_file():
        print("[freeze] missing metric JSON")
        return 3

    rep = parse_metric()
    REPORT.write_text(json.dumps(rep, indent=2))
    DONE.write_text("OMNIDOC_FULL_JOB_DONE\n" + json.dumps(rep, indent=2) + "\n")
    build_hf_package(rep)
    print("OMNIDOC_FULL_JOB_DONE")
    print(json.dumps(rep, indent=2))

    # Post-freeze A/B + table specialist (user requested)
    ab = ROOT / "tools" / "roe_omnidoc_ab_after_freeze.py"
    if ab.is_file():
        print("[freeze] starting post-freeze A/B + table specialist...", flush=True)
        subprocess.run(
            [str(ROOT / ".venv-unlimited-ocr" / "bin" / "python"), str(ab), "--force"],
            cwd=str(ROOT),
            check=False,
        )

    if args.publish:
        repo = os.environ.get("HF_REPO", "").strip()
        if not repo:
            print("[publish] set HF_REPO=org/name to upload")
            return 4
        if not rep.get("hf_ready") and not args.force:
            print("[publish] blocked: hf_ready false")
            return 5
        subprocess.check_call(
            ["huggingface-cli", "upload", repo, str(HF), ".", "--repo-type", "model"],
            cwd=str(ROOT),
        )
        print(f"[publish] uploaded {repo}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
