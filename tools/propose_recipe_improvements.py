#!/usr/bin/env python3
"""Emit bounded, evidence-backed recipe proposals (Phase 4).

Never proposes lowering certification bars. Appends JSONL rows compatible
with suggestions/cnet_compression_suggestions.jsonl shape so the existing
activate_cnet_suggestions consumer can grow to recipe kinds later.
"""
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

BANNED = {
    "lower_margin",
    "lower_wilson",
    "skip_certify",
    "force_admit",
    "disable_cert",
}

DEFAULT_PROPOSALS = [
    {
        "kind": "topk_set_v2",
        "priority": 5,
        "evidence": "qwythos margin sweep: set 255/256 vs ordered 101/256 at eps 0.02; new goldens required",
        "tags": ["recipe", "campaign", "topk_set", "never_lower_cert_bars"],
    },
    {
        "kind": "train_fast_default",
        "priority": 4,
        "evidence": "TEACH_FAST_PASS: 1.61x serial byte-identical plain-SGD",
        "tags": ["recipe", "deploy", "train_fast"],
    },
    {
        "kind": "teacher_sleep_cnb_primary",
        "priority": 4,
        "evidence": "CNET_TEACHER_IDLE_SEC + CNB serve; teacher only on open gaps",
        "tags": ["recipe", "deploy", "resource"],
    },
    {
        "kind": "budgeted_gap_drain",
        "priority": 3,
        "evidence": "CNET_LANE_MAX_CLOSURES rate-limits drain without skipping certify",
        "tags": ["recipe", "gap_lane", "budget"],
    },
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--out",
        default="suggestions/cnet_recipe_proposals.jsonl",
        help="JSONL output path",
    )
    ap.add_argument(
        "--kind",
        action="append",
        default=[],
        help="Only emit these kinds (default: all measured defaults)",
    )
    args = ap.parse_args()
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    kinds = set(args.kind) if args.kind else None
    n = 0
    with out.open("a", encoding="utf-8") as f:
        for row in DEFAULT_PROPOSALS:
            kind = row["kind"]
            if kind in BANNED:
                raise SystemExit(f"banned kind: {kind}")
            if kinds is not None and kind not in kinds:
                continue
            rec = {
                "id": f"cnet-recipe-{kind}",
                "kind": kind,
                "priority": row["priority"],
                "status": "proposed",
                "meta": {
                    "actionable": True,
                    "never_lower_cert_bars": True,
                    "source": "propose_recipe_improvements",
                },
                "evidence": row["evidence"],
                "tags": row["tags"],
                "ts": int(time.time()),
            }
            f.write(json.dumps(rec, sort_keys=True) + "\n")
            n += 1
    print(f"RECIPE_PROPOSALS_PASS wrote={n} path={out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
