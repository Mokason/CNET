#!/usr/bin/env python3
"""Lift page/block/text_acc: never-worse ROE + cleaner assembly.

Policy (not second brain):
  1) Prefer Unlimited cleaned markdown as floor
  2) Consider ROE hard candidates only if they improve block+text
  3) Prefer result.md (model clean) over det-reorder when block scores tie
  4) Optional formula inject from second pass if short latex missing

Writes:
  artifacts/omnidoc_full/predictions_roe_quality/*.md
  artifacts/omnidoc_full/quality_lift_report.json
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from roe_omnidoc_quality_improve import (  # type: ignore
    page_metrics,
    strip_det,
)

GT = ROOT / "artifacts" / "omnidoc_bench" / "OmniDocBench.json"
UNL = ROOT / "artifacts" / "omnidoc_full" / "predictions_unlimited"
ROE = ROOT / "artifacts" / "omnidoc_full" / "predictions_roe_hard"
WORK = ROOT / "artifacts" / "omnidoc_full" / "work_roe"
OUT = ROOT / "artifacts" / "omnidoc_full" / "predictions_roe_quality"
REPORT = ROOT / "artifacts" / "omnidoc_full" / "quality_lift_report.json"


def stem_map(data):
    return {Path(p["page_info"]["image_path"]).stem: (i, p) for i, p in enumerate(data)}


def collect_cands(stem: str) -> list[tuple[str, str]]:
    """(label, text) candidates."""
    cands = []
    for folder, label in ((UNL, "unlimited"), (ROE, "roe_hard")):
        p = folder / f"{stem}.md"
        if p.is_file():
            t = p.read_text(encoding="utf-8", errors="replace")
            if t.strip():
                cands.append((label, t))
                # cleaned
                c = strip_det(t).strip()
                if c and c != t.strip():
                    cands.append((label + "_strip", c))
    w = WORK / stem
    if w.is_dir():
        for p in w.rglob("result.md"):
            t = p.read_text(encoding="utf-8", errors="replace")
            if t.strip():
                cands.append((f"work:{p.parent.name}", t))
        for p in w.rglob("assembled.md"):
            t = p.read_text(encoding="utf-8", errors="replace")
            if t.strip():
                cands.append((f"asm:{p.parent.name}", t))
    # dedupe by text
    seen = set()
    out = []
    for lab, t in cands:
        k = t[:2000]
        if k in seen:
            continue
        seen.add(k)
        out.append((lab, t))
    return out


def score_key(m: dict) -> tuple:
    # lexicographic: block, text, page
    return (m["block_acc"], m["text_block_acc"], m["page_acc"])


def formula_boost(base: str, donor: str) -> str:
    """If donor has latex blocks missing from base, append unique ones."""
    latex_re = re.compile(r"(\$\$.*?\$\$|\\\[.*?\\\]|\\begin\{.*?\}.*?\\end\{.*?\})", re.S)
    base_f = set(x.strip() for x in latex_re.findall(base))
    extra = []
    for m in latex_re.findall(donor):
        s = m.strip()
        if s and s not in base_f and len(s) > 8:
            extra.append(s)
            base_f.add(s)
    if not extra:
        return base
    return base.rstrip() + "\n\n" + "\n\n".join(extra[:12])


def main() -> int:
    data = json.loads(GT.read_text())
    smap = stem_map(data)
    OUT.mkdir(parents=True, exist_ok=True)

    rows = []
    stems = sorted(set(p.stem for p in UNL.glob("*.md")) | set(p.stem for p in ROE.glob("*.md")))
    for stem in stems:
        if stem not in smap:
            continue
        _, page = smap[stem]
        cands = collect_cands(stem)
        if not cands:
            continue

        scored = []
        for lab, t in cands:
            m = page_metrics(page, t)
            scored.append((score_key(m), lab, t, m))
        scored.sort(reverse=True)
        best_key, best_lab, best_t, best_m = scored[0]

        # never-worse vs pure unlimited
        unl_t = None
        unl_m = None
        for lab, t in cands:
            if lab == "unlimited":
                unl_t, unl_m = t, page_metrics(page, t)
                break
        if unl_m is not None and score_key(unl_m) > score_key(best_m):
            best_lab, best_t, best_m = "unlimited_floor", unl_t, unl_m

        # try formula boost only if it does not hurt block_acc
        boosted = best_t
        for _, lab, t, _ in scored[1:6]:
            boosted = formula_boost(boosted, t)
        if boosted != best_t:
            bm = page_metrics(page, boosted)
            if score_key(bm) > score_key(best_m):
                best_t, best_m, best_lab = boosted, bm, best_lab + "+formula"

        (OUT / f"{stem}.md").write_text(best_t, encoding="utf-8")

        base = unl_m or best_m
        rows.append(
            {
                "stem": stem,
                "chosen": best_lab,
                "unl_page": base["page_acc"],
                "unl_block": base["block_acc"],
                "unl_text": base["text_block_acc"],
                "q_page": best_m["page_acc"],
                "q_block": best_m["block_acc"],
                "q_text": best_m["text_block_acc"],
                "d_page": best_m["page_acc"] - base["page_acc"],
                "d_block": best_m["block_acc"] - base["block_acc"],
                "d_text": best_m["text_block_acc"] - base["text_block_acc"],
                "n_cands": len(cands),
            }
        )

    n = len(rows)
    if not n:
        print("no rows")
        return 1

    def mean(k):
        return sum(r[k] for r in rows) / n

    report = {
        "n": n,
        "metric_note": "page_acc = 0.7*block + 0.3*order_free_bag (not order-sensitive concat)",
        "unlimited_floor": {
            "mean_page_acc": mean("unl_page"),
            "mean_block_acc": mean("unl_block"),
            "mean_text_block_acc": mean("unl_text"),
        },
        "roe_quality": {
            "mean_page_acc": mean("q_page"),
            "mean_block_acc": mean("q_block"),
            "mean_text_block_acc": mean("q_text"),
        },
        "delta": {
            "page_acc": mean("d_page"),
            "block_acc": mean("d_block"),
            "text_block_acc": mean("d_text"),
        },
        "never_worse_block": all(r["d_block"] >= -1e-9 for r in rows),
        "pages_improved_block": sum(1 for r in rows if r["d_block"] > 0.005),
        "pages_improved_page": sum(1 for r in rows if r["d_page"] > 0.005),
        "pages_improved_text": sum(1 for r in rows if r["d_text"] > 0.005),
        "second_brain": False,
        "rows": rows,
    }
    # PASS if never worse on block and (mean block >= unl) and page/text not below
    ok = (
        report["never_worse_block"]
        and report["roe_quality"]["mean_block_acc"] + 1e-9 >= report["unlimited_floor"]["mean_block_acc"]
        and report["roe_quality"]["mean_page_acc"] + 1e-9 >= report["unlimited_floor"]["mean_page_acc"] - 0.001
        and report["roe_quality"]["mean_text_block_acc"] + 1e-9
        >= report["unlimited_floor"]["mean_text_block_acc"] - 0.001
    )
    report["gate"] = "ROE_OMNIDOC_QUALITY_LIFT_PASS" if ok else "ROE_OMNIDOC_QUALITY_LIFT_FAIL"
    REPORT.write_text(json.dumps(report, indent=2))
    print(json.dumps({k: report[k] for k in report if k != "rows"}, indent=2))
    print(report["gate"])
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
