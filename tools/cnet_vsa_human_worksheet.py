#!/usr/bin/env python3
"""questions_human_worksheet.md (filled in) -> questions_human.tsv
TSV columns: gold[|alt|alt...]<TAB>question<TAB>section  (section: A=everyday, B=sibling, C=polysemy).
Alternates listed after ALT: become per-question valid targets (lenient top-1), applied to every system."""
import re, sys
from pathlib import Path
F = Path(sys.argv[1] if len(sys.argv) > 1 else "benchmarks/vsa_routing_arena_20260911")
names = {l.split("\t")[0] for l in (F / "corpora.tsv").read_text().splitlines()}
rows, section, gold = [], "", []
cur = {}
def flush():
    for k, (g, q, alts) in cur.items():
        if q.strip():
            bad = [a for a in alts if a not in names]
            if bad:
                sys.exit(f"unknown alternate capsule(s) {bad} in {k}")
            rows.append(f"{'|'.join([g] + alts)}\t{q.strip()}\t{section}")
    cur.clear()
for line in (F / "questions_human_worksheet.md").read_text().splitlines():
    m = re.match(r"^## ([ABC])\.", line)
    if m:
        flush(); section = m.group(1); continue
    m = re.match(r"^### ([ABC]\d+)\. (.+)$", line)
    if m:
        flush()
        head = m.group(2)
        if section == "A":
            gold = [head.strip()]
        elif section == "B":
            gold = [x.strip() for x in head.split("  vs  ")]
        else:
            gold = []
        continue
    m = re.match(r"^Q(\d?)\s*(?:\(([^)]+)\))?:\s*(.*)$", line)
    if m:
        k = m.group(1) or "1"
        g = m.group(2).strip() if m.group(2) else (gold[0] if gold else "")
        if g not in names:
            sys.exit(f"unknown capsule '{g}' at: {line}")
        cur[k] = (g, m.group(3), [])
        continue
    m = re.match(r"^ALT(\d?):\s*(.*)$", line)
    if m and (m.group(1) or "1") in cur:
        k = m.group(1) or "1"; g, q, _ = cur[k]
        cur[k] = (g, q, [a.strip() for a in m.group(2).split(",") if a.strip()])
flush()
out = F / "questions_human.tsv"
out.write_text("# Human-written post-freeze set. gold[|alt...]<TAB>question<TAB>section(A everyday / B sibling / C polysemy). Never trained on.\n" + "\n".join(rows) + "\n")
print(f"wrote {len(rows)} questions -> {out}  (A {sum(r.endswith('A') for r in rows)}, B {sum(r.endswith('B') for r in rows)}, C {sum(r.endswith('C') for r in rows)})")
