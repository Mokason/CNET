#!/usr/bin/env python3
"""Turn verification demand into the next mining window.

Reads a <base>.usage.jsonl ledger (written by CnetMcpServer per
cnet_verify_claim call) and emits a CNET_WINDOW_FILE of the most-requested
token ids the base could NOT serve — uncovered claim tokens, weighted by
request frequency. Already-certified ids (any --exclude window/ledger) are
filtered out, so successive windows grow coverage instead of re-mining it.

Usage:
  usage_window.py <usage.jsonl> [-V 256] [--exclude window.txt ...]
                  [--tokenizer-dir DIR] [-o out.txt]

Output: one token id per line (mine with CNET_WINDOW_FILE=<out>), plus a
human-readable .words.txt sidecar when a tokenizer.json is available.
"""

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

DEFAULT_TOKENIZER_DIR = "/home/marble/AI/Models/gemma-4-31B-it-int4-AutoRound"


def load_excludes(paths):
    seen = set()
    for p in paths:
        for line in Path(p).read_text().splitlines():
            line = line.strip()
            if not line:
                continue
            tok = line.split()[0]
            if tok.isdigit():
                seen.add(int(tok))
            elif tok.startswith("tk"):  # gaps ledger form tk<id>q<id>
                digits = tok[2:].split("q")[0]
                if digits.isdigit():
                    seen.add(int(digits))
    return seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("usage", help="<base>.usage.jsonl ledger")
    ap.add_argument("-V", type=int, default=256, help="window size")
    ap.add_argument("--exclude", action="append", default=[],
                    help="window file or gaps ledger of already-mined ids")
    ap.add_argument("--tokenizer-dir", default=DEFAULT_TOKENIZER_DIR)
    ap.add_argument("-o", "--out", default=None)
    args = ap.parse_args()

    demand = Counter()
    lines = 0
    for raw in Path(args.usage).read_text().splitlines():
        raw = raw.strip()
        if not raw:
            continue
        try:
            entry = json.loads(raw)
        except json.JSONDecodeError:
            continue
        lines += 1
        for tid in entry.get("uncovered", []):
            demand[int(tid)] += 1

    excluded = load_excludes(args.exclude)
    ranked = [(tid, n) for tid, n in demand.most_common() if tid not in excluded]
    if len(ranked) < args.V:
        print(f"note: only {len(ranked)} uncovered ids in demand "
              f"(from {lines} verifications); window will be short of "
              f"V={args.V} — mine it with V={len(ranked)}", file=sys.stderr)
    picked = ranked[: args.V]
    if not picked:
        print("no uncovered demand recorded — nothing to mine", file=sys.stderr)
        return 1

    out = Path(args.out or (str(Path(args.usage)).replace(
        ".usage.jsonl", "") + f".demand_window_{len(picked)}.txt"))
    out.write_text("".join(f"{tid}\n" for tid, _ in picked))

    # words sidecar when the tokenizer vocab is at hand
    tok_json = Path(args.tokenizer_dir) / "tokenizer.json"
    if tok_json.exists():
        vocab = json.loads(tok_json.read_text())["model"]["vocab"]
        id2tok = {v: k for k, v in vocab.items()}
        side = out.with_suffix(".words.txt")
        side.write_text("".join(
            f"{tid}\t{id2tok.get(tid, '?')}\t{n}\n" for tid, n in picked))
        print(f"wrote {out} and {side}")
    else:
        print(f"wrote {out}")
    print(f"demand: {len(demand)} distinct uncovered ids over {lines} "
          f"verifications; window: {len(picked)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
