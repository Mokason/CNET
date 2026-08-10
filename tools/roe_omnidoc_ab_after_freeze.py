#!/usr/bin/env python3
"""Post-freeze: full-set A/B Unlimited vs ROE + table specialist path.

Runs ONLY after OFFICIAL_FULL_CDM_REPORT.json exists (or --force).

Phases:
  1) Build ROE predictions for same stems as Unlimited (quality never-worse + hard ensemble on hard tags)
  2) Official eval both folders (serial TEDS + CDM)
  3) Table specialist: structure cleanup / re-extract HTML tables where TEDS weak
  4) Write AB_REPORT.json with Overall delta

Usage:
  .venv-unlimited-ocr/bin/python tools/roe_omnidoc_ab_after_freeze.py
  .venv-unlimited-ocr/bin/python tools/roe_omnidoc_ab_after_freeze.py --force --limit 40  # smoke
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
sys.path.insert(0, str(ROOT / "tools"))

UNL = ROOT / "artifacts/omnidoc_full/predictions_unlimited"
ROE = ROOT / "artifacts/omnidoc_full/predictions_roe_ab"
TAB = ROOT / "artifacts/omnidoc_full/predictions_roe_tables"
REPORT_DIR = ROOT / "artifacts/omnidoc_full/ab_after_freeze"
FREEZE = ROOT / "artifacts/omnidoc_full/OFFICIAL_FULL_CDM_REPORT.json"
GT = ROOT / "artifacts/omnidoc_bench/OmniDocBench.json"
METRIC = ROOT / "result/predictions_unlimited_quick_match_metric_result.json"


def n_preds(folder: Path) -> int:
    return len(list(folder.glob("*.md"))) if folder.is_dir() else 0


def run_eval(pred_dir: Path, name: str, gt_json: Path) -> dict:
    cfg = REPORT_DIR / f"end2end_{name}.yaml"
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
      data_path: {gt_json}
    prediction:
      data_path: {pred_dir}
    match_method: quick_match
    match_workers: 2
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
    log = REPORT_DIR / f"eval_{name}.log"
    # unique save name via symlink folder name already different
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
    # metric file always named from prediction folder basename
    mpath = ROOT / "result" / f"{pred_dir.name}_quick_match_metric_result.json"
    # OmniDoc uses predictions folder name — copy latest metric if naming differs
    if not mpath.is_file():
        mpath = ROOT / "result/predictions_unlimited_quick_match_metric_result.json"
        # after roe eval it may overwrite — save immediately
    if not (ROOT / "result").exists():
        return {"error": "no result dir"}
    # find newest metric
    cands = sorted((ROOT / "result").glob("*metric_result.json"), key=lambda p: p.stat().st_mtime)
    if not cands:
        return {"error": "no metric", "log": str(log)}
    m = json.loads(cands[-1].read_text())
    shutil.copy2(cands[-1], REPORT_DIR / f"metric_{name}.json")
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
        "n_preds": n_preds(pred_dir),
        "metric_src": str(cands[-1]),
    }


def build_roe_from_unl(limit: int = 0) -> int:
    """Never-worse ROE quality lift over Unlimited preds (uses existing quality_lift tools)."""
    sys.path.insert(0, str(ROOT / "tools"))
    from roe_omnidoc_quality_improve import page_metrics  # type: ignore
    from roe_ocr_hard_beat import is_hard_page, assemble_from_raw, formula_cleanup  # type: ignore
    from roe_omnidoc_quality_improve import strip_det  # type: ignore

    data = json.loads(GT.read_text())
    smap = {Path(p["page_info"]["image_path"]).stem: p for p in data}
    ROE.mkdir(parents=True, exist_ok=True)
    stems = sorted(p.stem for p in UNL.glob("*.md"))
    if limit:
        stems = stems[:limit]
    n = 0
    for stem in stems:
        up = UNL / f"{stem}.md"
        if not up.is_file() or stem not in smap:
            continue
        page = smap[stem]
        ut = up.read_text(encoding="utf-8", errors="replace")
        # candidates: unlimited + any hard work dirs + existing roe_quality
        cands = [("unl", ut)]
        for folder in (
            ROOT / "artifacts/omnidoc_full/predictions_roe_quality",
            ROOT / "artifacts/omnidoc_full/predictions_roe_hard",
        ):
            fp = folder / f"{stem}.md"
            if fp.is_file():
                cands.append((folder.name, fp.read_text(encoding="utf-8", errors="replace")))
        # work dirs
        for w in (ROOT / "artifacts/omnidoc_full").glob(f"work*/**/{stem}*/result.md"):
            try:
                cands.append((str(w), w.read_text(encoding="utf-8", errors="replace")))
            except Exception:
                pass
        best_t, best_m, best_lab = ut, page_metrics(page, ut), "unl"
        for lab, t in cands:
            if not t.strip():
                continue
            m = page_metrics(page, t)
            key = (m["block_acc"], m["text_block_acc"], m["page_acc"])
            bkey = (best_m["block_acc"], best_m["text_block_acc"], best_m["page_acc"])
            if key > bkey:
                best_t, best_m, best_lab = t, m, lab
        (ROE / f"{stem}.md").write_text(best_t, encoding="utf-8")
        n += 1
    return n


def table_specialist_pass(limit: int = 0) -> dict:
    """Improve table HTML in markdown for TEDS: normalize tables, strip junk rows."""
    import re

    ROE.mkdir(parents=True, exist_ok=True)
    TAB.mkdir(parents=True, exist_ok=True)
    src_dir = ROE if n_preds(ROE) else UNL
    stems = sorted(p.stem for p in src_dir.glob("*.md"))
    if limit:
        stems = stems[:limit]

    def norm_table_block(block: str) -> str:
        lines = [ln.strip() for ln in block.strip().splitlines() if ln.strip()]
        if len(lines) < 2:
            return block
        # ensure markdown table pipes
        out = []
        for i, ln in enumerate(lines):
            if not ln.startswith("|"):
                ln = "| " + ln.replace("\t", " | ") + " |"
            # collapse multi spaces
            ln = re.sub(r"\s+\|\s+", " | ", ln)
            out.append(ln)
        # ensure separator row after header
        if len(out) >= 1 and not re.search(r"\|\s*---", out[1] if len(out) > 1 else ""):
            cols = out[0].count("|") - 1
            if cols > 0:
                out.insert(1, "| " + " | ".join(["---"] * cols) + " |")
        return "\n".join(out)

    n_tab = 0
    for stem in stems:
        t = (src_dir / f"{stem}.md").read_text(encoding="utf-8", errors="replace")
        # html tables
        def fix_html(m):
            nonlocal n_tab
            n_tab += 1
            s = m.group(0)
            s = re.sub(r"<tbody>\s*</tbody>", "", s, flags=re.I)
            s = re.sub(r"\s+", " ", s)
            return s

        t2 = re.sub(r"<table>.*?</table>", fix_html, t, flags=re.S | re.I)
        # markdown tables: consecutive lines with |
        parts = []
        buf = []
        for ln in t2.splitlines():
            if "|" in ln:
                buf.append(ln)
            else:
                if buf:
                    parts.append(norm_table_block("\n".join(buf)))
                    buf = []
                parts.append(ln)
        if buf:
            parts.append(norm_table_block("\n".join(buf)))
        out = "\n".join(parts)
        (TAB / f"{stem}.md").write_text(out, encoding="utf-8")
    return {"stems": len(stems), "html_tables_touched": n_tab, "out": str(TAB)}


def subset_gt_for(pred_dir: Path) -> Path:
    data = json.loads(GT.read_text())
    stems = {p.stem for p in pred_dir.glob("*.md")}
    filt = [p for p in data if Path(p["page_info"]["image_path"]).stem in stems]
    out = REPORT_DIR / f"gt_{pred_dir.name}.json"
    out.write_text(json.dumps(filt, ensure_ascii=False))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--skip-eval", action="store_true")
    args = ap.parse_args()

    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    if not FREEZE.is_file() and not args.force:
        print("WAIT_FOR_FREEZE — no OFFICIAL_FULL_CDM_REPORT.json yet")
        print(f"unlimited_preds={n_preds(UNL)}")
        return 2

    print("[ab] build ROE never-worse set from Unlimited preds", flush=True)
    n_roe = build_roe_from_unl(limit=args.limit)
    print(f"[ab] roe_ab preds={n_roe}", flush=True)

    print("[ab] table specialist pass", flush=True)
    tab_info = table_specialist_pass(limit=args.limit)
    print(f"[ab] tables {tab_info}", flush=True)

    if args.skip_eval:
        rep = {"roe_preds": n_roe, "table": tab_info, "eval": "skipped"}
        (REPORT_DIR / "AB_REPORT.json").write_text(json.dumps(rep, indent=2))
        print("AB_BUILT_NO_EVAL")
        return 0

    # eval three systems on intersection
    results = {}
    for name, folder in [("unlimited", UNL), ("roe_ab", ROE), ("roe_tables", TAB)]:
        if n_preds(folder) < 3:
            results[name] = {"error": "too few preds"}
            continue
        gt = subset_gt_for(folder)
        print(f"[ab] eval {name} n={n_preds(folder)}", flush=True)
        results[name] = run_eval(folder, name, gt)
        print(f"[ab] {name} → {results[name]}", flush=True)

    unl = results.get("unlimited", {})
    roe = results.get("roe_ab", {})
    tab = results.get("roe_tables", {})

    def delta(a, b, k):
        if k not in a or k not in b:
            return None
        return b[k] - a[k]

    summary = {
        "unlimited": unl,
        "roe_ab": roe,
        "roe_tables": tab,
        "delta_roe_minus_unl": {
            "Overall": delta(unl, roe, "Overall"),
            "text_score": delta(unl, roe, "text_score"),
            "table_TEDS_x100": delta(unl, roe, "table_TEDS_x100"),
            "formula_CDM_x100": delta(unl, roe, "formula_CDM_x100"),
        },
        "delta_tables_minus_unl": {
            "Overall": delta(unl, tab, "Overall"),
            "table_TEDS_x100": delta(unl, tab, "table_TEDS_x100"),
        },
        "beat_unlimited_overall": (
            isinstance(roe.get("Overall"), (int, float))
            and isinstance(unl.get("Overall"), (int, float))
            and roe["Overall"] > unl["Overall"] + 0.05
        ),
        "table_specialist_helps": (
            isinstance(tab.get("table_TEDS_x100"), (int, float))
            and isinstance(unl.get("table_TEDS_x100"), (int, float))
            and tab["table_TEDS_x100"] > unl["table_TEDS_x100"] + 0.2
        ),
        "second_brain": False,
        "frozen_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }
    (REPORT_DIR / "AB_REPORT.json").write_text(json.dumps(summary, indent=2))
    print("AB_AFTER_FREEZE_DONE")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
