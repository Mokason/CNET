#!/usr/bin/env python3
"""Pick genuinely-teachable window tokens and queue them as NO_PLAN gaps.

Shared by scripts/cnet_autoteach_tick.sh and scripts/governor_autonomous.py so
the two injectors cannot drift apart.

Two bugs this replaces:

1. Quantized seeding. Both callers did `random.seed(int(time.time()) // N)`
   (3600s in the tick, 600s in the governor), which makes the sample identical
   for the whole bucket. The autoteach timer fires every 20 min, so all three
   ticks in an hour injected the SAME four token ids; the lane closed them on
   the first pass and reported `bound=0 / drained=0` for the other two. The
   effective ceiling was ~4 new tokens per hour regardless of capacity.

2. No coverage filter. Ids already sealed as units (or already sitting in the
   inbox) were re-injected and silently discarded downstream, so a tick could
   look busy while proposing nothing new.

Coverage is read from the gap ledger's closed rows (state field == "2") rather
than from cnb_audit, because the ledger is a cheap text read and is the same
source the lane dedupes against.
"""
from __future__ import annotations

import argparse
import os
import random
import re
import sys
from pathlib import Path

TOKEN_RE = re.compile(r"tk(\d+)q\1\b")


def window_ids(window_file: Path) -> list[int]:
    if not window_file.exists():
        return []
    return [int(x) for x in window_file.read_text().split() if x.strip().isdigit()]


def covered_ids(gaps_path: Path) -> set[int]:
    """Token ids with a closed (state == 2) row in the ledger."""
    out: set[int] = set()
    if not gaps_path.exists():
        return out
    for i, line in enumerate(gaps_path.read_text(errors="replace").splitlines()):
        if i < 2:
            continue
        parts = line.split()
        if len(parts) > 1 and parts[1] == "2":
            m = TOKEN_RE.search(line)
            if m:
                out.add(int(m.group(1)))
    return out


def pending_ids(inbox: Path) -> set[int]:
    out: set[int] = set()
    if not inbox.exists():
        return out
    for line in inbox.read_text(errors="replace").splitlines():
        m = TOKEN_RE.search(line)
        if m:
            out.add(int(m.group(1)))
    return out


def pick(base: Path, window_file: Path, n: int, k: int = 3) -> tuple[list[str], dict]:
    ids = window_ids(window_file)
    if not ids:
        return [], {"reason": "empty_window", "window": str(window_file)}
    width = len(ids)
    skip = covered_ids(Path(str(base) + ".gaps.txt")) | pending_ids(
        Path(str(base) + ".inbox")
    )
    fresh = [i for i in ids if i not in skip]
    exhausted = 0
    if not fresh:
        # Whole window is sealed or queued: say so rather than re-proposing
        # covered ids, which is what made ticks look productive while idle.
        return [], {
            "reason": "window_exhausted",
            "window": str(window_file),
            "width": width,
            "covered": len(skip),
        }
    if len(fresh) < n:
        exhausted = 1
    # No fixed seed: every call draws independently.
    picks = random.sample(fresh, min(n, len(fresh)))
    lines = [f"NO_PLAN 1 {width} 1 w_cur 1 {width} {k} tk{t}q{t}\n" for t in picks]
    return lines, {
        "reason": "ok",
        "width": width,
        "covered": len(skip),
        "remaining": len(fresh) - len(picks),
        "near_exhausted": exhausted,
    }


def main() -> int:
    root = Path(os.environ.get("CNET_ROOT", Path(__file__).resolve().parents[1]))
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=int(os.environ.get("CNET_AUTOTEACH_INJECT", "4")))
    ap.add_argument("--k", type=int, default=int(os.environ.get("CNET_CURIOSITY_K", "3")))
    ap.add_argument("--base", default=os.environ.get("CNET_BASE_PATH", str(root / "soul_gemma4v2_final.cnb")))
    ap.add_argument(
        "--window",
        default=os.environ.get("CNET_WINDOW_FILE")
        or os.environ.get("CNET_RESIDUAL_WINDOW")
        or str(root / "english_window_256_bonsai_v2.txt"),
    )
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    base = Path(args.base)
    inbox = Path(os.environ.get("CNET_GAP_INBOX", str(base) + ".inbox"))
    lines, info = pick(base, Path(args.window), args.n, args.k)
    if lines and not args.dry_run:
        inbox.parent.mkdir(parents=True, exist_ok=True)
        with inbox.open("a") as f:
            f.writelines(lines)
    print(
        f"AUTOTEACH_INJECT n={len(lines)} reason={info.get('reason')} "
        f"covered={info.get('covered', 0)} remaining={info.get('remaining', 0)} inbox={inbox}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
