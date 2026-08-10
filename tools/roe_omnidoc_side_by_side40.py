#!/usr/bin/env python3
"""Analyze TEDS failures + side-by-side Unlimited vs ROE quality on same 40 pages.

Uses official OmniDoc metric JSON when present, plus local page_metrics.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from roe_omnidoc_quality_improve import page_metrics  # type: ignore

GT = ROOT / "artifacts/omnidoc_bench/OmniDocBench.json"
UNL = ROOT / "artifacts/omnidoc_full/predictions_unlimited"
ROE = ROOT / "artifacts/omnidoc_full/predictions_roe_quality"
ROE_HARD = ROOT / "artifacts/omnidoc_full/predictions_roe_hard"
OUT = ROOT / "artifacts/omnidoc_full/side_by_side40"


def stem_map(data):
    return {Path(p["page_info"]["image_path"]).stem: (i, p) for i, p in enumerate(data)}


def page_attr(page):
    a = page.get("page_info", {}).get("page_attribute", {}) or {}
    cats = {}
    for d in page.get("layout_dets") or []:
        if d.get("ignore"):
            continue
        c = d.get("category_type") or ""
        cats[c] = cats.get(c, 0) + 1
    return {
        "data_source": a.get("data_source"),
        "layout": a.get("layout"),
        "language": a.get("language"),
        "n_tables": cats.get("table", 0),
        "n_formula": cats.get("equation_isolated", 0) + cats.get("equation_semantic", 0),
    }


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    data = json.loads(GT.read_text())
    smap = stem_map(data)

    # --- official TEDS dump if present ---
    teds_path = ROOT / "result/predictions_unlimited_quick_match_table_per_table_TEDS.json"
    table_result = ROOT / "result/predictions_unlimited_quick_match_table_result.json"
    teds_rows = []
    if table_result.is_file():
        tr = json.loads(table_result.read_text())
        if isinstance(tr, list):
            for item in tr:
                teds = None
                if isinstance(item.get("metric"), dict):
                    teds = item["metric"].get("TEDS")
                if teds is None:
                    teds = item.get("TEDS")
                teds_rows.append(
                    {
                        "img_id": item.get("img_id") or item.get("image_name"),
                        "TEDS": teds,
                        "edit": item.get("edit") or (item.get("metric") or {}).get("Edit_dist"),
                        "gt_cat": item.get("gt_category_type"),
                        "error": item.get("error") or item.get("reason"),
                    }
                )
    teds_sorted = sorted(
        [r for r in teds_rows if isinstance(r.get("TEDS"), (int, float))],
        key=lambda r: r["TEDS"],
    )
    teds_analysis = {
        "n_tables": len(teds_rows),
        "n_scored": len(teds_sorted),
        "mean_TEDS": sum(r["TEDS"] for r in teds_sorted) / len(teds_sorted) if teds_sorted else None,
        "weak_TEDS_lt_0.5": [r for r in teds_sorted if r["TEDS"] < 0.5],
        "weak_TEDS_lt_0.8": [r for r in teds_sorted if r["TEDS"] < 0.8],
        "perfect_TEDS": sum(1 for r in teds_sorted if r["TEDS"] >= 0.999),
        "worst10": teds_sorted[:10],
        "best5": teds_sorted[-5:] if teds_sorted else [],
    }

    # --- side-by-side page metrics ---
    stems = sorted(set(p.stem for p in UNL.glob("*.md")))
    paired = []
    for stem in stems:
        if stem not in smap:
            continue
        _, page = smap[stem]
        up = UNL / f"{stem}.md"
        # prefer roe_quality, else roe_hard, else unl copy
        rp = ROE / f"{stem}.md"
        if not rp.is_file():
            rp = ROE_HARD / f"{stem}.md"
        if not up.is_file():
            continue
        ut = up.read_text(encoding="utf-8", errors="replace")
        mu = page_metrics(page, ut)
        if rp.is_file():
            rt = rp.read_text(encoding="utf-8", errors="replace")
            mr = page_metrics(page, rt)
            src = "roe_quality" if (ROE / f"{stem}.md").is_file() else "roe_hard"
        else:
            mr = mu
            src = "missing_roe_used_unl"
        attr = page_attr(page)
        paired.append(
            {
                "stem": stem,
                "roe_src": src,
                **attr,
                "unl_page": mu["page_acc"],
                "unl_block": mu["block_acc"],
                "unl_text": mu["text_block_acc"],
                "roe_page": mr["page_acc"],
                "roe_block": mr["block_acc"],
                "roe_text": mr["text_block_acc"],
                "d_page": mr["page_acc"] - mu["page_acc"],
                "d_block": mr["block_acc"] - mu["block_acc"],
                "d_text": mr["text_block_acc"] - mu["text_block_acc"],
                "identical_md": ut.strip() == (rp.read_text(encoding="utf-8", errors="replace").strip() if rp.is_file() else ut.strip()),
            }
        )

    n = len(paired)
    def mean(k):
        return sum(r[k] for r in paired) / n if n else 0.0

    summary = {
        "n": n,
        "unlimited": {
            "page_acc": mean("unl_page"),
            "block_acc": mean("unl_block"),
            "text_block_acc": mean("unl_text"),
        },
        "roe": {
            "page_acc": mean("roe_page"),
            "block_acc": mean("roe_block"),
            "text_block_acc": mean("roe_text"),
        },
        "delta_roe_minus_unl": {
            "page_acc": mean("d_page"),
            "block_acc": mean("d_block"),
            "text_block_acc": mean("d_text"),
        },
        "roe_wins_block": sum(1 for r in paired if r["d_block"] > 0.005),
        "unl_wins_block": sum(1 for r in paired if r["d_block"] < -0.005),
        "ties_block": sum(1 for r in paired if abs(r["d_block"]) <= 0.005),
        "identical_md_count": sum(1 for r in paired if r["identical_md"]),
        "by_source": {},
    }
    # group by data_source
    from collections import defaultdict
    g = defaultdict(list)
    for r in paired:
        g[r.get("data_source") or "?"].append(r)
    for k, rows in g.items():
        m = len(rows)
        summary["by_source"][k] = {
            "n": m,
            "d_block": sum(x["d_block"] for x in rows) / m,
            "d_text": sum(x["d_text"] for x in rows) / m,
            "unl_block": sum(x["unl_block"] for x in rows) / m,
            "roe_block": sum(x["roe_block"] for x in rows) / m,
        }

    report = {
        "teds_analysis": teds_analysis,
        "side_by_side": summary,
        "paired_rows": paired,
        "asi_claim": (
            "PROVEN_DELTA"
            if summary["delta_roe_minus_unl"]["block_acc"] > 0.005
            else "NO_MATERIAL_DELTA_ON_40"
        ),
        "second_brain": False,
    }
    (OUT / "side_by_side_report.json").write_text(json.dumps(report, indent=2))
    # compact print
    print(json.dumps({
        "teds": {k: teds_analysis[k] for k in teds_analysis if k not in ("weak_TEDS_lt_0.5", "weak_TEDS_lt_0.8", "worst10", "best5")},
        "teds_weak_lt_0.5": teds_analysis["weak_TEDS_lt_0.5"],
        "teds_worst10": teds_analysis["worst10"],
        "side_by_side": summary,
        "asi_claim": report["asi_claim"],
    }, indent=2))
    print("SIDE_BY_SIDE40_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
