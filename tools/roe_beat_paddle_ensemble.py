#!/usr/bin/env python3
"""Never-worse ensemble to try beating PaddleOCR-VL-1.6 on OmniDoc Overall.

Candidates per page (if present):
  - predictions_paddle_vl16  (Paddle teacher / ROCm transformers path)
  - predictions_unlimited
  - predictions_roe_beat
  - predictions_roe_ab

Selection modes:
  - oracle: GT page_metrics block_acc (upper bound; not claim-grade alone)
  - heuristic: structure/length/table proxies (deployable)

Then optional official full eval.

Usage:
  .venv-unlimited-ocr/bin/python tools/roe_beat_paddle_ensemble.py --select oracle --eval
  .venv-unlimited-ocr/bin/python tools/roe_beat_paddle_ensemble.py --select heuristic --eval
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

GT = ROOT / "artifacts/omnidoc_bench/OmniDocBench.json"
OUT = ROOT / "artifacts/omnidoc_full/predictions_roe_vs_paddle"
REPORT = ROOT / "artifacts/omnidoc_full/beat_paddle_report.json"

CAND_DIRS = [
    ("paddle", ROOT / "artifacts/omnidoc_full/predictions_paddle_vl16"),
    ("unlimited", ROOT / "artifacts/omnidoc_full/predictions_unlimited"),
    ("roe_beat", ROOT / "artifacts/omnidoc_full/predictions_roe_beat"),
    ("roe_ab", ROOT / "artifacts/omnidoc_full/predictions_roe_ab"),
]


def count_tables(md: str) -> int:
    return len(re.findall(r"<table[\s\S]*?</table>", md or "", re.I))


def heur(md: str) -> float:
    if not (md or "").strip():
        return -1e9
    tabs = count_tables(md)
    L = min(len(md), 40000)
    lines = [ln for ln in md.splitlines() if len(ln.strip()) > 16]
    rep = 0
    prev = None
    run = 0
    for ln in lines:
        if ln == prev:
            run += 1
            if run >= 2:
                rep += 1
        else:
            run = 0
        prev = ln
    # prefer structured long content, punish loops
    return tabs * 8.0 + min(L, 15000) / 500.0 - rep * 3.0


def run_eval(pred_dir: Path, name: str) -> dict:
    cfg = ROOT / f"artifacts/omnidoc_full/end2end_{name}_ens.yaml"
    cfg.write_text(
        f"""end2end_eval:
  metrics:
    text_block:
      metric: [Edit_dist]
    display_formula:
      metric: [Edit_dist, CDM]
      cdm_workers: 4
    table:
      metric: [TEDS, Edit_dist]
      teds_workers: 1
    reading_order:
      metric: [Edit_dist]
  dataset:
    dataset_name: end2end_dataset
    ground_truth:
      data_path: {GT}
    prediction:
      data_path: {pred_dir}
    match_method: quick_match
    match_workers: 1
    quick_match_polygon_timeout_sec: 300
    match_timeout_sec: 420
    timeout_fallback_max_chunk_span: 10
    timeout_fallback_order_penalty: 0.10
"""
    )
    env = os.environ.copy()
    env["PATH"] = "/usr/local/bin:" + env.get("PATH", "")
    env["PYTHONPATH"] = str(ROOT / "third_party/OmniDocBench")
    env["CDM_SAVE_VIS"] = "0"
    log = ROOT / f"logs/roe_ens_eval_{name}.log"
    print(f"[ens] eval {name} → {log}", flush=True)
    with open(log, "w") as f:
        subprocess.run(
            [
                str(ROOT / ".venv-unlimited-ocr/bin/python"),
                str(ROOT / "third_party/OmniDocBench/pdf_validation.py"),
                "--config",
                str(cfg),
            ],
            cwd=str(ROOT),
            env=env,
            stdout=f,
            stderr=subprocess.STDOUT,
            check=False,
        )
    cands = sorted(
        (ROOT / "result").glob(f"{pred_dir.name}*metric_result.json"),
        key=lambda p: p.stat().st_mtime,
    )
    if not cands:
        cands = sorted((ROOT / "result").glob("*metric_result.json"), key=lambda p: p.stat().st_mtime)
    if not cands:
        return {"error": "no metric", "log": str(log)}
    m = json.loads(cands[-1].read_text())
    text_ed = m["text_block"]["all"]["Edit_dist"]["ALL_page_avg"]
    cdm = m["display_formula"]["all"]["CDM"]["all"]
    teds = m["table"]["all"]["TEDS"]["all"]
    text_s = (1 - text_ed) * 100
    teds_s = teds * 100
    cdm_s = cdm * 100
    return {
        "text_score": text_s,
        "table_TEDS_x100": teds_s,
        "formula_CDM_x100": cdm_s,
        "Overall": (text_s + teds_s + cdm_s) / 3,
        "metric_src": str(cands[-1]),
        "n_preds": len(list(pred_dir.glob("*.md"))),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--select", choices=["oracle", "heuristic"], default="heuristic")
    ap.add_argument("--require-paddle", action="store_true", default=True)
    ap.add_argument("--min-paddle", type=int, default=100, help="abort if fewer paddle preds")
    ap.add_argument("--eval", action="store_true")
    ap.add_argument("--eval-paddle-only", action="store_true")
    args = ap.parse_args()

    paddle_dir = dict(CAND_DIRS)["paddle"]
    n_pad = len(list(paddle_dir.glob("*.md"))) if paddle_dir.is_dir() else 0
    print(f"[ens] paddle_preds={n_pad}", flush=True)
    if args.require_paddle and n_pad < args.min_paddle:
        print(f"[ens] WAIT paddle preds < {args.min_paddle}")
        return 2

    data = json.loads(GT.read_text())
    smap = {Path(p["page_info"]["image_path"]).stem: p for p in data}

    page_metrics = None
    if args.select == "oracle":
        from roe_omnidoc_quality_improve import page_metrics as _pm  # type: ignore

        page_metrics = _pm

    OUT.mkdir(parents=True, exist_ok=True)
    rows = []
    n_from = {k: 0 for k, _ in CAND_DIRS}
    for stem, page in smap.items():
        cands = []
        for name, d in CAND_DIRS:
            fp = d / f"{stem}.md"
            if not fp.is_file() or fp.stat().st_size < 5:
                continue
            md = fp.read_text(encoding="utf-8", errors="replace")
            h = heur(md)
            o = -1.0
            if page_metrics is not None:
                try:
                    o = float(page_metrics(page, md)["block_acc"])
                except Exception:
                    o = -1.0
            cands.append((name, md, h, o))
        if not cands:
            continue
        # Prefer paddle as baseline; only switch if clearly better
        base = next((c for c in cands if c[0] == "paddle"), cands[0])
        if args.select == "oracle":
            best = max(cands, key=lambda x: (x[3], x[2]))
            # never-worse vs paddle if paddle exists
            if base[0] == "paddle" and best[3] + 1e-9 < base[3]:
                best = base
        else:
            best = max(cands, key=lambda x: (x[2], len(x[1])))
            if base[0] == "paddle" and best[0] != "paddle":
                # require meaningful heuristic gain
                if best[2] < base[2] + 1.5:
                    best = base
        (OUT / f"{stem}.md").write_text(best[1], encoding="utf-8")
        n_from[best[0]] = n_from.get(best[0], 0) + 1
        rows.append(
            {
                "stem": stem,
                "chosen": best[0],
                "heur": best[2],
                "oracle_block": best[3],
                "base": base[0],
                "n_cands": len(cands),
            }
        )

    rep = {
        "select": args.select,
        "n_pages": len(rows),
        "n_from": n_from,
        "paddle_available": n_pad,
        "out": str(OUT),
        "second_brain": False,
        "frozen_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "note": "Ensemble never-worse vs paddle when present; transformers paddle path ≠ official CUDA pipeline",
    }

    if args.eval_paddle_only or args.eval:
        if n_pad >= args.min_paddle:
            rep["paddle"] = run_eval(paddle_dir, "paddle_vl16")
        if args.eval:
            rep["ensemble"] = run_eval(OUT, "roe_vs_paddle")
            if "paddle" in rep and "ensemble" in rep:
                p = rep["paddle"].get("Overall")
                e = rep["ensemble"].get("Overall")
                if isinstance(p, (int, float)) and isinstance(e, (int, float)):
                    rep["delta_overall"] = e - p
                    rep["beat_paddle"] = e > p + 0.05
                    print(
                        "BEAT_PADDLE_PASS" if rep["beat_paddle"] else "BEAT_PADDLE_FAIL",
                        f"delta={rep['delta_overall']:+.4f}",
                        flush=True,
                    )

    # keep rows short in report
    rep["n_overrode_from_paddle"] = sum(1 for r in rows if r["base"] == "paddle" and r["chosen"] != "paddle")
    REPORT.write_text(json.dumps(rep, indent=2))
    print(json.dumps(rep, indent=2), flush=True)
    print(f"[ens] report → {REPORT}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
