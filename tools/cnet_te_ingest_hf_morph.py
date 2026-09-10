#!/usr/bin/env python3
"""One-shot harvest: HuggingFace wiktionary-morph → typed_en gold TSV.

External labels, not CNET mouth. Does not CERT by itself — cnetd loads the
TSV as overlay (same path as operator gold last). Skip regular -ed.

  python3 tools/cnet_te_ingest_hf_morph.py \\
      --parquet /tmp/hf-wiktionary-morph/en/train-00000-of-00001.parquet \\
      --out $CNET_PACKS_ROOT/en_irregular_gold.tsv
"""
from __future__ import annotations

import argparse
import os
import re
import sys

BAD = {
    "archaic",
    "obsolete",
    "nonstandard",
    "dialectal",
    "alternative",
    "table-tags",
    "inflection-template",
    "error-unknown-tag",
}
JUNK = {
    "to",
    "up",
    "yes",
    "um",
    "fat",
    "got",
    "in",
    "on",
    "out",
    "off",
    "down",
    "like",
    "back",
    "over",
    "about",
    "must",
    "might",
    "gan",
    "it",
    "if",
    "as",
    "or",
    "an",
    "no",
    "so",
}
WORD = re.compile(r"^[a-z]+$")


def as_list(x):
    if x is None:
        return []
    if hasattr(x, "tolist"):
        x = x.tolist()
    if isinstance(x, (list, tuple)):
        return list(x)
    return [x]


def clean(s) -> str:
    if not isinstance(s, str):
        return ""
    s = s.strip().lower()
    return s if WORD.match(s) else ""


def doubled_regular(lemma: str, past: str) -> bool:
    return len(lemma) >= 2 and past == lemma + lemma[-1] + "ed"


def harvest(parquet_path: str, min_freq: int) -> list[tuple[str, str, str, int]]:
    import pyarrow.parquet as pq

    table = pq.read_table(
        parquet_path, columns=["word", "pos", "freq", "inflection_forms"]
    )
    df = table.to_pandas()
    best: dict[str, tuple[int, str, str]] = {}
    for r in df.itertuples(index=False):
        if "verb" not in [str(p) for p in as_list(r.pos)]:
            continue
        lemma = clean(str(r.word) if r.word is not None else "")
        if not lemma or lemma in JUNK:
            continue
        if len(lemma) < 3 and lemma not in {"be", "do", "go"}:
            continue
        pasts, parts = [], []
        for f in as_list(r.inflection_forms):
            try:
                f = dict(f)
            except Exception:
                continue
            if str(f.get("pos", "")) != "verb":
                continue
            tags = {str(x) for x in as_list(f.get("tags"))}
            form = clean(str(f.get("form", "")))
            if not form:
                continue
            if tags & BAD:
                continue
            is_past = "past" in tags
            is_part = "participle" in tags
            is_pres = "present" in tags
            if is_past and not is_part and not is_pres:
                pasts.append(form)
            if is_past and is_part and not is_pres:
                parts.append(form)
        if not pasts:
            continue
        past = pasts[0]
        part = parts[0] if parts else past
        regular = lemma + "d" if lemma.endswith("e") else lemma + "ed"
        if past == regular or doubled_regular(lemma, past):
            continue
        if lemma.endswith("y") and past == lemma[:-1] + "ied":
            continue
        if past.endswith("cked"):
            continue
        freq = int(r.freq) if r.freq == r.freq else 0
        if freq < min_freq:
            continue
        prev = best.get(lemma)
        if prev is None or freq > prev[0]:
            best[lemma] = (freq, past, part)
    rows = sorted(((k, v[1], v[2], v[0]) for k, v in best.items()), key=lambda x: -x[3])
    return rows


def harvest_unimorph(path: str) -> list[tuple[str, str, str, int]]:
    """UniMorph eng: lemma\\tform\\tV;PST / V;V.PTCP;PST. Irregulars only."""
    past: dict[str, str] = {}
    part: dict[str, str] = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            cols = line.rstrip("\n").split("\t")
            if len(cols) < 3:
                continue
            lemma, form, tag = cols[0].lower(), cols[1].lower(), cols[2]
            lemma, form = clean(lemma), clean(form)
            if not lemma or not form or lemma in JUNK:
                continue
            if tag == "V;PST":
                past.setdefault(lemma, form)
            elif tag == "V;V.PTCP;PST":
                part.setdefault(lemma, form)
    rows = []
    for lemma, pst in past.items():
        regular = lemma + "d" if lemma.endswith("e") else lemma + "ed"
        if pst == regular or doubled_regular(lemma, pst):
            continue
        if lemma.endswith("y") and pst == lemma[:-1] + "ied":
            continue
        if pst.endswith("cked"):
            continue
        prt = part.get(lemma, pst)
        rows.append((lemma, pst, prt, 0))
    rows.sort(key=lambda r: r[0])
    return rows


def merge_rows(*groups: list[tuple[str, str, str, int]]) -> list[tuple[str, str, str, int]]:
    """Later groups add lemmas and fill empty past/part. Never overwrite nonempty."""
    best: dict[str, tuple[str, str, str, int]] = {}
    for rows in groups:
        for lemma, past, part, freq in rows:
            if lemma not in best:
                best[lemma] = (lemma, past, part, freq)
                continue
            _, op, orp, of = best[lemma]
            if not op and past:
                op = past
            if not orp and part:
                orp = part
            if freq > of:
                of = freq
            best[lemma] = (lemma, op, orp, of)
    return sorted(best.values(), key=lambda r: (-r[3], r[0]))


def merge_existing(path: str, rows: list[tuple[str, str, str, int]]):
    extra = []
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split("\t")
                if len(parts) < 2 or not parts[0]:
                    continue
                lemma, past = parts[0], parts[1]
                part = parts[2] if len(parts) > 2 else ""
                extra.append((lemma, past, part, 0))
    return merge_rows(extra, rows)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--parquet", default="")
    ap.add_argument("--unimorph", default="", help="UniMorph eng TSV (HF unimorph/universal_morphologies eng)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--min-freq", type=int, default=40)
    args = ap.parse_args()
    if not args.parquet and not args.unimorph:
        print("need --parquet and/or --unimorph", file=sys.stderr)
        return 2
    groups: list[list[tuple[str, str, str, int]]] = []
    src = []
    if args.parquet:
        groups.append(harvest(args.parquet, args.min_freq))
        src.append("wiktionary-morph")
    if args.unimorph:
        groups.append(harvest_unimorph(args.unimorph))
        src.append("unimorph-eng")
    rows = merge_rows(*groups)
    rows = merge_existing(args.out, rows)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("# lemma\tpast\tpart\n")
        f.write(f"# source: {' + '.join(src)}; overlay only; never self-CERT\n")
        for lemma, past, part, freq in rows:
            f.write(f"{lemma}\t{past}\t{part}\n")
    print(f"wrote {len(rows)} rows -> {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
