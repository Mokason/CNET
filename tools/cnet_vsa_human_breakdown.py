#!/usr/bin/env python3
"""Post-freeze set (human or independent-model): strict and lenient top-1 per section (A everyday / B sibling / C polysemy) for every CNET row and Qwen, from the arena dump
(kind extra3 = the fourth --extra-questions file) and Qwen's per-question tops. Lenient = gold, per-question alternates, capsule alternates."""
import sys, glob
from pathlib import Path
F = Path(sys.argv[1]); dump = Path(sys.argv[2])
setname = sys.argv[3] if len(sys.argv) > 3 else "questions_human.tsv"   # any gold|alt<TAB>q<TAB>section set
kind = sys.argv[4] if len(sys.argv) > 4 else "extra3"                    # its position among the --extra-questions files
rows = [l.split("\t") for l in (F / setname).read_text().splitlines() if l and not l.startswith("#")]
alts = {}
for l in (F / "alternates.tsv").read_text().splitlines():
    if l and not l.startswith("#"):
        a, b = l.split("\t")[:2]; alts.setdefault(a, set()).add(b)
gold, qalt, sect = {}, {}, {}
for f in rows:
    g = f[0].split("|"); gold[f[1]] = g[0]; qalt[f[1]] = set(g[1:]); sect[f[1]] = f[2] if len(f) > 2 else "?"
def ok(q, t): return t == gold[q] or t in qalt[q] or t in alts.get(gold[q], ())
res = {}
for l in dump.read_text().splitlines()[1:]:
    f = l.split("\t")
    if f[1] == kind and f[10] in gold:
        res.setdefault(f[0], {})[f[10]] = (f[3], f[4])
for p in glob.glob("var/arena_cache/qwen3-embedding-4b/extra_*_top.tsv"):
    for l in Path(p).read_text().splitlines():
        f = l.split("\t")
        if f[5] in gold:
            res.setdefault("qwen3-4b", {})[f[5]] = (f[1], f[2])
print("\n| system | section | n | strict top-1 | lenient top-1 | lenient top-2 |")
print("|---|---|---|---|---|---|")
for sysname, r in res.items():
    for s in ("A", "B", "C", "all"):
        qs = [q for q in gold if (sect[q] == s or s == "all") and q in r]
        if not qs: continue
        t1 = sum(r[q][0] == gold[q] for q in qs) / len(qs); l1 = sum(ok(q, r[q][0]) for q in qs) / len(qs); l2 = sum(ok(q, r[q][0]) or ok(q, r[q][1]) for q in qs) / len(qs)
        print(f"| {sysname} | {s} | {len(qs)} | {100*t1:.1f}% | {100*l1:.1f}% | {100*l2:.1f}% |")
