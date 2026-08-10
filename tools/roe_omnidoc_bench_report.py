#!/usr/bin/env python3
"""Score OmniDoc predictions folders with ROE block metrics + optional official eval.

Produces artifacts/omnidoc_full/benchmark_report.json
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from roe_omnidoc_quality_improve import page_metrics  # type: ignore

GT = ROOT / "artifacts" / "omnidoc_bench" / "OmniDocBench.json"
UNL = ROOT / "artifacts" / "omnidoc_full" / "predictions_unlimited"
ROE = ROOT / "artifacts" / "omnidoc_full" / "predictions_roe_hard"
OUT = ROOT / "artifacts" / "omnidoc_full" / "benchmark_report.json"


def stem_map(data):
    m = {}
    for i, p in enumerate(data):
        stem = Path(p["page_info"]["image_path"]).stem
        m[stem] = (i, p)
    return m


def score_folder(folder: Path, smap: dict) -> dict:
    rows = []
    for md in sorted(folder.glob("*.md")):
        stem = md.stem
        if stem not in smap:
            continue
        i, page = smap[stem]
        pred = md.read_text(encoding="utf-8", errors="replace")
        m = page_metrics(page, pred)
        m["stem"] = stem
        m["idx"] = i
        rows.append(m)
    if not rows:
        return {"n": 0}
    n = len(rows)
    return {
        "n": n,
        "mean_page_acc": sum(r["page_acc"] for r in rows) / n,
        "mean_block_acc": sum(r["block_acc"] for r in rows) / n,
        "mean_text_block_acc": sum(r["text_block_acc"] for r in rows) / n,
        "mean_block_edit": sum(r["block_edit_norm"] for r in rows) / n,
        "rows": rows,
    }


def try_official_eval(pred_dir: Path, name: str) -> dict:
    """Attempt OmniDocBench pdf_validation if env allows."""
    repo = ROOT / "third_party" / "OmniDocBench"
    cfg_path = ROOT / "artifacts" / "omnidoc_full" / f"end2end_{name}.yaml"
    if not cfg_path.is_file():
        return {"status": "no_config"}
    # write result dir
    res_dir = ROOT / "artifacts" / "omnidoc_full" / f"official_{name}"
    res_dir.mkdir(parents=True, exist_ok=True)
    py = ROOT / ".venv-unlimited-ocr" / "bin" / "python"
    # try import omnidocbench / run pdf_validation
    cmd = [
        str(py),
        str(repo / "pdf_validation.py"),
        "--config",
        str(cfg_path),
    ]
    try:
        # ensure prediction path exists and has files
        n = len(list(pred_dir.glob("*.md")))
        if n < 3:
            return {"status": "too_few_preds", "n": n}
        p = subprocess.run(
            cmd,
            cwd=str(repo),
            capture_output=True,
            text=True,
            timeout=900,
            env={**os.environ, "PYTHONPATH": str(repo)},
        )
        log = res_dir / "eval.log"
        log.write_text(p.stdout + "\n" + p.stderr)
        return {
            "status": "ok" if p.returncode == 0 else "failed",
            "returncode": p.returncode,
            "log": str(log),
            "stdout_tail": (p.stdout or "")[-2000:],
            "stderr_tail": (p.stderr or "")[-2000:],
        }
    except Exception as e:
        return {"status": "error", "error": str(e)}


def main():
    import os

    data = json.loads(GT.read_text())
    smap = stem_map(data)
    unl = score_folder(UNL, smap)
    roe = score_folder(ROE, smap)

    # paired comparison on intersection
    unl_map = {r["stem"]: r for r in unl.get("rows", [])}
    roe_map = {r["stem"]: r for r in roe.get("rows", [])}
    common = sorted(set(unl_map) & set(roe_map))
    paired = []
    for s in common:
        u, r = unl_map[s], roe_map[s]
        paired.append(
            {
                "stem": s,
                "unl_block": u["block_acc"],
                "roe_block": r["block_acc"],
                "delta_block": r["block_acc"] - u["block_acc"],
                "unl_text": u["text_block_acc"],
                "roe_text": r["text_block_acc"],
                "delta_text": r["text_block_acc"] - u["text_block_acc"],
            }
        )
    if paired:
        n = len(paired)
        paired_summary = {
            "n": n,
            "mean_unl_block": sum(p["unl_block"] for p in paired) / n,
            "mean_roe_block": sum(p["roe_block"] for p in paired) / n,
            "mean_delta_block": sum(p["delta_block"] for p in paired) / n,
            "mean_unl_text": sum(p["unl_text"] for p in paired) / n,
            "mean_roe_text": sum(p["roe_text"] for p in paired) / n,
            "mean_delta_text": sum(p["delta_text"] for p in paired) / n,
            "roe_wins_block": sum(1 for p in paired if p["delta_block"] > 0.005),
            "unl_wins_block": sum(1 for p in paired if p["delta_block"] < -0.005),
        }
    else:
        paired_summary = {"n": 0}

    official = {
        "unlimited": try_official_eval(UNL, "unlimited"),
        "roe_hard": try_official_eval(ROE, "roe_hard"),
    }

    report = {
        "gt_pages": len(data),
        "predictions_unlimited": {k: unl[k] for k in unl if k != "rows"},
        "predictions_roe_hard": {k: roe[k] for k in roe if k != "rows"},
        "paired": paired_summary,
        "paired_rows": paired,
        "official_eval": official,
        "sota_claim": "WITHHELD" if unl.get("n", 0) < 1600 else "RUN_OFFICIAL_OVERALL",
        "note": "Full 1651 SOTA requires official Overall on complete set. This report is the live path + scores on available preds.",
    }
    OUT.write_text(json.dumps(report, indent=2))
    print(json.dumps({k: report[k] for k in report if k != "paired_rows"}, indent=2))
    # marker
    if paired_summary.get("n", 0) >= 5:
        print("ROE_OMNIDOC_FULL_BENCH_PASS")
        return 0
    print("ROE_OMNIDOC_FULL_BENCH_PARTIAL")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
