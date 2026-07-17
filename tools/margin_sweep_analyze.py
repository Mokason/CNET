#!/usr/bin/env python3
"""Analyze a CNET_MARGIN_SWEEP TSV: choose the v2 yield lever.

Reproduces the acquire oracle-evidence gate offline (usable/attempts >=
evidence_threshold AND usable >= min_evidence) for BOTH abstention
criteria — ordered top-3 decision margin (today's semantics) and top-3
SET margin (CNET_TOPK_SET) — across an epsilon grid, and validates the
ordered@campaign-eps prediction against the actual campaign ledger.

Usage:
  margin_sweep_analyze.py <sweep.tsv> [--ledger <base.gaps.txt>]
                          [--eps 0.02] [--evidence 0.9] [--min-evidence 16]

Evidence label: this predicts the ORACLE-EVIDENCE gate only. A predicted-
minable unit can still defer downstream (certify_failed, class_imbalance);
the campaign measured those at ~2% of attempts, so the prediction is a
tight upper bound and the ledger validation quantifies the gap.
"""
import argparse
import re
import sys
from collections import defaultdict


def read_sweep(path):
    per_unit = defaultdict(list)  # token -> [(ordered, set), ...]
    header = None
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                header = line.strip()
                continue
            if line.startswith("unit_idx"):
                continue
            p = line.split()
            if len(p) != 6:
                continue
            per_unit[int(p[1])].append((float(p[4]), float(p[5])))
    return header, per_unit


def read_ledger(path):
    """token -> 'acquired' | defer reason

    Acquired detection must cover BOTH ledger generations: v2+ rows carry
    the minted unit name (" acq_<tag>"); v1 rows have no unit column and an
    acquired row simply ends at an empty defer-reason ("... cce_cond_next -").
    """
    out = {}
    with open(path) as f:
        for line in f:
            m = re.search(r"\btk(\d+)q\d+\b", line)
            if not m:
                continue
            tok = int(m.group(1))
            if " acq_" in line or line.rstrip().endswith(" -"):
                out[tok] = "acquired"
            else:
                cols = line.split()
                # defer reason sits after the oracle name column
                reason = "deferred"
                for r in ("oracle_unfit", "insufficient_exemplars",
                          "certify_failed", "class_imbalance",
                          "unbounded_domain", "incumbent_healthy"):
                    if r in cols:
                        reason = r
                        break
                out[tok] = reason
    return out


def gate(margins, idx, eps, evidence, min_evidence):
    n = len(margins)
    usable = sum(1 for m in margins if m[idx] >= eps)
    if usable == 0:
        return "oracle_unfit", usable
    if usable < min_evidence:
        return "insufficient_exemplars", usable
    if usable / n < evidence:
        return "oracle_unfit", usable
    return "minable", usable


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tsv")
    ap.add_argument("--ledger")
    ap.add_argument("--eps", type=float, default=0.02)
    ap.add_argument("--evidence", type=float, default=0.9)
    ap.add_argument("--min-evidence", type=int, default=16)
    ap.add_argument("--semantics", choices=("set", "ordered"), default="set",
                    help="which margin gate to use for --export-minable")
    ap.add_argument("--export-minable", metavar="PATH",
                    help="write one predicted-minable token id per line "
                         "(for CNET_UNIT_ALLOWLIST)")
    ap.add_argument("--export-unfit", metavar="PATH",
                    help="write predicted-unfit token ids (diagnostics)")
    a = ap.parse_args()

    header, per_unit = read_sweep(a.tsv)
    if not per_unit:
        sys.exit("no sweep rows in %s" % a.tsv)
    print(header or "(no header)")
    # completeness: the acquire gate divides by attempts == V (the full
    # window). A partial unit (truncated / still-being-written TSV) would
    # gate over a window-ordered prefix — drop it loudly instead.
    full = max(len(v) for v in per_unit.values())
    partial = [t for t, v in per_unit.items() if len(v) != full]
    for t in partial:
        del per_unit[t]
    if partial:
        print("WARNING: dropped %d partial unit(s) (< %d probes): %s"
              % (len(partial), full,
                 ", ".join("tk%d" % t for t in sorted(partial)[:8])))
    if not per_unit:
        sys.exit("no complete units")
    nu = len(per_unit)
    nprobe = full
    print("units=%d probes/unit=%d" % (nu, nprobe))

    # eps sweep table
    grid = [0.005, 0.01, 0.02, 0.03, 0.05, 0.08, 0.12]
    if a.eps not in grid:
        grid = sorted(set(grid + [a.eps]))
    print("\npredicted minable units (evidence>=%.2f, min_evidence=%d):"
          % (a.evidence, a.min_evidence))
    print("%-8s %12s %12s %10s" % ("eps", "ordered", "set", "recovered"))
    for eps in grid:
        yo = sum(1 for t in per_unit
                 if gate(per_unit[t], 0, eps, a.evidence,
                         a.min_evidence)[0] == "minable")
        ys = sum(1 for t in per_unit
                 if gate(per_unit[t], 1, eps, a.evidence,
                         a.min_evidence)[0] == "minable")
        mark = "  <-- campaign eps" if abs(eps - a.eps) < 1e-12 else ""
        print("%-8g %8d/%-3d %8d/%-3d %+10d%s"
              % (eps, yo, nu, ys, nu, ys - yo, mark))

    # per-unit verdicts at campaign eps
    rows = []
    for t in sorted(per_unit):
        vo, uo = gate(per_unit[t], 0, a.eps, a.evidence, a.min_evidence)
        vs, us = gate(per_unit[t], 1, a.eps, a.evidence, a.min_evidence)
        mean_set = sum(m[1] for m in per_unit[t]) / len(per_unit[t])
        rows.append((t, vo, uo, vs, us, mean_set))

    conv = [r for r in rows if r[1] != "minable" and r[3] == "minable"]
    lost = [r for r in rows if r[1] == "minable" and r[3] != "minable"]
    print("\nat eps=%g: set-semantics converts %d units, loses %d"
          % (a.eps, len(conv), len(lost)))

    # Export allowlist for CNET_UNIT_ALLOWLIST (quality-preserving screen).
    sem_idx = 3 if a.semantics == "set" else 1  # vs / vo field in rows
    minable_toks = sorted(r[0] for r in rows if r[sem_idx] == "minable")
    unfit_toks = sorted(r[0] for r in rows if r[sem_idx] != "minable")
    print("\nexport semantics=%s @ eps=%g: minable=%d unfit=%d"
          % (a.semantics, a.eps, len(minable_toks), len(unfit_toks)))
    if a.export_minable:
        with open(a.export_minable, "w") as f:
            f.write("# predicted-minable token ids (%s eps=%g evidence>=%.2f "
                    "min_ev=%d)\n"
                    % (a.semantics, a.eps, a.evidence, a.min_evidence))
            for t in minable_toks:
                f.write("%d\n" % t)
        print("wrote allowlist %s (%d tokens)" % (a.export_minable,
                                                  len(minable_toks)))
    if a.export_unfit:
        with open(a.export_unfit, "w") as f:
            f.write("# predicted-unfit token ids (%s eps=%g)\n"
                    % (a.semantics, a.eps))
            for t in unfit_toks:
                f.write("%d\n" % t)
        print("wrote unfit list %s (%d tokens)" % (a.export_unfit,
                                                   len(unfit_toks)))

    # decisiveness ranking (what WINDOW_SCREEN would sort by)
    rows.sort(key=lambda r: -r[5])
    print("\ntop 10 / bottom 10 by mean set margin (screen decisiveness):")
    for r in rows[:10]:
        print("  tk%-7d mean_set=%8.4f ordered=%s set=%s" % (r[0], r[5], r[1], r[3]))
    print("  ...")
    for r in rows[-10:]:
        print("  tk%-7d mean_set=%8.4f ordered=%s set=%s" % (r[0], r[5], r[1], r[3]))

    if a.ledger:
        led = read_ledger(a.ledger)
        tp = fp = fn = tn = 0
        fps, fns = [], []
        for (t, vo, uo, vs, us, _) in rows:
            actual = led.get(t)
            if actual is None:
                continue
            pred_min = vo == "minable"
            act_min = actual == "acquired"
            if pred_min and act_min:
                tp += 1
            elif pred_min and not act_min:
                fp += 1
                fps.append((t, actual, uo))
            elif not pred_min and act_min:
                fn += 1
                fns.append((t, uo))
            else:
                tn += 1
        print("\nvalidation vs ledger %s (ordered @ eps=%g):" % (a.ledger, a.eps))
        print("  predicted-minable & acquired:      %3d (true positive)" % tp)
        print("  predicted-minable & deferred:      %3d (downstream defers)" % fp)
        print("  predicted-unfit   & acquired:      %3d (should be ~0)" % fn)
        print("  predicted-unfit   & deferred:      %3d (true negative)" % tn)
        for t, reason, uo in fps[:12]:
            print("    fp tk%d actual=%s usable=%d" % (t, reason, uo))
        for t, uo in fns[:12]:
            print("    fn tk%d usable=%d" % (t, uo))


if __name__ == "__main__":
    main()
