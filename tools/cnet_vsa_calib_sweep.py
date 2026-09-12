#!/usr/bin/env python3
"""Evaluate radius-calibration separability across many corpora for several
target pairs, from the distance reports written by
`cnet_vsa_cli gencap-create ... --dry-run --calib-report <file>`.

Each report holds the sorted leave-one-out in-domain distances and the sorted
cross-domain negative distances of one corpus. For a target pair
(accept >= t_in, reject >= t_neg) a corpus is separable when the tightest
radius admitting t_in of the in-domain set is not above the loosest radius
rejecting t_neg of the negatives, which is exactly the rule the C library
applies. Reports the separable fraction, the chosen radius distribution and
the worst offenders.

Usage: tools/cnet_vsa_calib_sweep.py <reports_dir> [--out result.md]
"""
import argparse
import math
import statistics
from pathlib import Path

PAIRS = [(0.90, 0.95), (0.90, 0.90), (0.85, 0.95), (0.85, 0.90), (0.80, 0.95), (0.80, 0.90), (0.75, 0.90), (0.70, 0.90)]


def load(path):
    name, d_in, d_neg = None, [], []
    for line in Path(path).read_text().splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "name":
            name = parts[1] if len(parts) > 1 else path.stem
        elif parts[0] == "in":
            d_in = [float(x) for x in parts[1:]]
        elif parts[0] == "neg":
            d_neg = [float(x) for x in parts[1:]]
    return name, d_in, d_neg


def window(d_in, d_neg, t_in, t_neg):
    n, m = len(d_in), len(d_neg)
    k = max(1, min(n, math.ceil(t_in * n)))
    r_in = d_in[k - 1]
    j = min(m - 1, int(math.floor((1.0 - t_neg) * m + 1e-9)))
    r_neg = d_neg[j] - 1e-4
    return r_in, r_neg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reports_dir")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    corpora = []
    for p in sorted(Path(args.reports_dir).glob("*.txt")):
        name, d_in, d_neg = load(p)
        if len(d_in) >= 8 and len(d_neg) >= 8:
            corpora.append((name, d_in, d_neg))

    lines = [f"# Radius calibration sweep over {len(corpora)} corpora\n"]
    in_means = [statistics.fmean(d) for _, d, _ in corpora]
    neg_means = [statistics.fmean(d) for _, _, d in corpora]
    lines.append(f"in-domain leave-one-out distance: mean of means {statistics.fmean(in_means):.3f}; "
                 f"negatives: mean of means {statistics.fmean(neg_means):.3f}\n")
    lines.append("| accept >= | reject >= | separable | chosen radius median | radius p10 | radius p90 |")
    lines.append("|---|---|---|---|---|---|")
    per_pair = {}
    for t_in, t_neg in PAIRS:
        ok, radii, failed = 0, [], []
        for name, d_in, d_neg in corpora:
            r_in, r_neg = window(d_in, d_neg, t_in, t_neg)
            if r_in <= r_neg:
                ok += 1
                radii.append(0.5 * (r_in + r_neg))
            else:
                failed.append((r_in - r_neg, name))
        per_pair[(t_in, t_neg)] = (ok, radii, failed)
        if radii:
            radii.sort()
            p10 = radii[int(0.10 * (len(radii) - 1))]
            p90 = radii[int(0.90 * (len(radii) - 1))]
            med = radii[len(radii) // 2]
        else:
            p10 = p90 = med = float("nan")
        lines.append(f"| {t_in:.2f} | {t_neg:.2f} | {ok}/{len(corpora)} ({ok/len(corpora):.1%}) | {med:.3f} | {p10:.3f} | {p90:.3f} |")
    lines.append("")

    # what does a corpus need to be separable at reject 0.95: the achievable accept
    lines.append("## Achievable in-domain accept at reject >= 0.95, per corpus")
    achievable = []
    for name, d_in, d_neg in corpora:
        m = len(d_neg)
        j = min(m - 1, int(math.floor(0.05 * m + 1e-9)))
        r_neg = d_neg[j] - 1e-4
        acc = sum(1 for d in d_in if d <= r_neg) / len(d_in)
        achievable.append((acc, name))
    achievable.sort()
    accs = [a for a, _ in achievable]
    lines.append(f"median achievable accept {accs[len(accs)//2]:.2f}; p10 {accs[int(0.1*(len(accs)-1))]:.2f}; p90 {accs[int(0.9*(len(accs)-1))]:.2f}")
    lines.append("")
    lines.append("Worst 15 corpora (least separable):")
    lines.append("| corpus | achievable accept |")
    lines.append("|---|---|")
    for acc, name in achievable[:15]:
        lines.append(f"| {name} | {acc:.2f} |")
    text = "\n".join(lines) + "\n"
    print(text)
    if args.out:
        Path(args.out).write_text(text)


if __name__ == "__main__":
    main()
