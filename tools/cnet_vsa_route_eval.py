#!/usr/bin/env python3
"""Fail-closed VSA router eval.

In-domain queries, in order, never mixing sources in one rate:
  1. var/distill/<name>_probes.txt (held-out teacher questions)
  2. last two lines of var/distill/<name>_corpus.txt (held out ONLY with --reseal-heldout)
  3. capsule name — only with --allow-name-queries, reported as src=name

OOD queries:
  - the alien list AFTER dropping any item whose 4+ letter content tokens
    overlap a capsule name (those are covered topics, not aliens)
  - plus sentences from distill corpora that have no matching .gencap

Score: correct - 2 * wrong_accept. Abstain is 0. This is the arena rule.
A transformer comparison is not this tool; see cnet_vsa_vs_transformer.py.

Usage: tools/cnet_vsa_route_eval.py [--dir bin] [--limit N] [--out result.md]
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

CLI = os.environ.get("CNET_CLI", "bin/cnet_vsa_cli")
WRONG_COST = 2
STOP = {
    "what", "which", "where", "when", "that", "this", "with", "from", "have",
    "been", "their", "there", "some", "more", "does", "about", "used", "how",
    "into", "than", "then", "them", "they", "your", "will", "would",
    "could", "should", "before", "after", "through", "between", "under", "over",
    "standard", "best", "way", "give", "gives", "lead", "stop", "using",
}
# Tokens that appear in many capsule names and must not mark a query "covered".
GENERIC_NAME = {
    "open", "closed", "shape", "pattern", "system", "design", "model", "analysis",
    "process", "control", "energy", "power", "water", "plant", "data", "time",
    "rate", "flow", "high", "low", "first", "based", "also", "type", "unit",
    "test", "form", "case", "area", "line", "work", "state", "field",
}


def distinctive_overlap(a, b):
    return {t for t in (content_tokens(a) & content_tokens(b)) if t not in GENERIC_NAME and len(t) >= 6}

ALIEN = [
    "How do I fold and shape a sourdough loaf for an open crumb?",
    "How do quantum qubits maintain coherent superposition on a Bloch sphere before decoherence?",
    "What stitch pattern gives a Fair Isle sweater its stranded colorwork?",
    "How are deep-sea coral reef ecosystems affected by ocean acidification and thermal bleaching?",
    "What are the astrological interpretations of planetary transits through the twelfth zodiac house?",
    "How does a haute couture atelier drape silk velvet for an evening gown?",
    "How do paleontologists date dinosaur fossils in sedimentary strata?",
    "What gives a wheated bourbon its sweetness during barrel aging?",
    "Which chess openings lead to a closed center and a kingside pawn storm?",
    "How do you tune a violin using harmonics and a fifth interval?",
    "What is the best way to train a puppy to stop pulling on the leash?",
    "How does a sommelier detect cork taint in an aged Burgundy?",
]

RX_RADIUS = re.compile(r"Radius gate:\s+dist=([0-9.]+) (<=|>) limit=([0-9.]+) -> (pass|REFUSE)")
RX_MARGIN = re.compile(r"Margin gate:\s+z=(-?[0-9.]+) (>=|<) z_min=([0-9.]+).*-> (pass|REFUSE)")
RX_AMB = re.compile(r"Ambiguity gate:\s+gap=([0-9.]+) (>=|<) .* -> (pass|REFUSE)")
RX_TERM = re.compile(r"Term gate:\s+(.*?) -> (pass|REFUSE)")
RX_SPACE = re.compile(r"Routing space:\s+(\S+)")
RX_WINNER = re.compile(r"DISPATCH TO '([^']+)'")
RX_CLOSEST = re.compile(r"closest='([^']+)'")


def content_tokens(text):
    toks = re.findall(r"[a-z]{4,}", text.lower().replace("_", " "))
    return {t for t in toks if t not in STOP}


def parse_route_block(out):
    mr = RX_RADIUS.search(out)
    mm = RX_MARGIN.search(out)
    if not mr:
        return None
    ma = RX_AMB.search(out)
    mt = RX_TERM.search(out)
    msp = RX_SPACE.search(out)
    radius_ok = mr.group(4) == "pass"
    margin_ok = (mm.group(4) == "pass") if mm else True
    amb_ok = (ma.group(3) == "pass") if ma else True
    term_ok = (mt.group(2) == "pass") if mt else True   # term-dependence gate (wide space; absent = not run)
    z = float(mm.group(1)) if mm else float("nan")
    zmin = float(mm.group(3)) if mm else float("nan")
    win = RX_WINNER.search(out)
    close = RX_CLOSEST.search(out)
    name = win.group(1) if win else (close.group(1) if close else "?")
    return {
        "dist": float(mr.group(1)),
        "limit": float(mr.group(3)),
        "radius_ok": radius_ok,
        "margin_ok": margin_ok,
        "z": z,
        "zmin": zmin,
        "legacy_accept": radius_ok,
        "new_accept": radius_ok and margin_ok and amb_ok and term_ok,
        "term_ok": term_ok,
        "top": name,
        "space": msp.group(1) if msp else "?",
    }


def route(reg_dir, query):
    out = subprocess.run([CLI, "route", reg_dir, query], capture_output=True, text=True).stdout
    return parse_route_block(out)


def route_many(reg_dir, queries):
    """Route all queries with ONE registry load (route-batch). Returns a list
    parallel to queries; None where the CLI produced no parseable block."""
    if not queries:
        return []
    text = "\n".join(q.replace("\n", " ") for q in queries) + "\n"
    out = subprocess.run([CLI, "route-batch", reg_dir], input=text, capture_output=True, text=True).stdout
    blocks = re.split(r"^=== QUERY \d+ ===$", out, flags=re.M)[1:]
    results = [parse_route_block(b) for b in blocks]
    while len(results) < len(queries):
        results.append(None)
    return results[: len(queries)]


def in_domain_queries(name, probes_dir, allow_name):
    pf = Path(probes_dir) / f"{name}_probes.txt"
    cf = Path(probes_dir) / f"{name}_corpus.txt"
    if pf.exists():
        qs = [l.strip() for l in pf.read_text().splitlines() if l.strip()][:2]
        if qs:
            return qs, "probes"
    if cf.exists():
        lines = [l.strip() for l in cf.read_text().splitlines() if l.strip()]
        hold = lines[-2:] if len(lines) >= 4 else lines[-1:]
        if hold:
            return hold, "corpus"
    if allow_name:
        return [name.replace("_", " ")], "name"
    return [], "none"


HOLDOUT = 2  # last N corpus sentences held out of the seal and used as queries


def reseal_heldout(src_dir, probes_dir, out_dir, encoder, target_in, target_neg, limit, skip=0):
    """Reseal every capsule of src_dir from its corpus MINUS the last HOLDOUT
    sentences, calibrated, into out_dir. Returns the capsule names sealed.
    Capsules without a corpus, or refused by calibration, are skipped and
    counted so the report can say so."""
    import subprocess as sp
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    # stale capsules from an earlier run (other encoder, other format, refused
    # since) would be routed against as if they were part of this reseal
    for old in out.glob("*.gencap"):
        old.unlink()
    trim = out / "_trim"
    trim.mkdir(exist_ok=True)
    names = sorted(p.stem for p in Path(src_dir).glob("*.gencap"))
    names = names[skip:]
    if limit:
        names = names[:limit]
    # Global split: the negatives directory must not contain ANY capsule's
    # held-out sentences or evaluation probes, or a query could be sampled as a
    # negative when calibrating a competing capsule. Every corpus in probes_dir
    # is copied minus its last HOLDOUT lines; probe files minus their first 2.
    negdir = out / "_negatives"
    negdir.mkdir(exist_ok=True)
    for old in negdir.glob("*.txt"):
        old.unlink()
    for cf in Path(probes_dir).glob("*_corpus.txt"):
        ls = [l for l in cf.read_text().splitlines() if l.strip()]
        if len(ls) > HOLDOUT:
            (negdir / cf.name).write_text("\n".join(ls[:-HOLDOUT]) + "\n")
    for pf in Path(probes_dir).glob("*_probes.txt"):
        ls = [l for l in pf.read_text().splitlines() if l.strip()]
        if len(ls) > 2:
            (negdir / pf.name).write_text("\n".join(ls[2:]) + "\n")
    sealed, refused, missing = [], [], []
    for i, name in enumerate(names, 1):
        cf = Path(probes_dir) / f"{name}_corpus.txt"
        if not cf.exists():
            missing.append(name)
            continue
        lines = [l for l in cf.read_text().splitlines() if l.strip()]
        if len(lines) < 12:
            missing.append(name)
            continue
        tf = trim / f"{name}_corpus.txt"
        tf.write_text("\n".join(lines[:-HOLDOUT]) + "\n")
        cmd = [CLI, "gencap-create", name, "HELDOUT", str(tf), str(out / f"{name}.gencap"),
               "--encoder", encoder, "--negatives", str(negdir),
               "--target-in", str(target_in), "--target-neg", str(target_neg)]
        pf = Path(probes_dir) / f"{name}_probes.txt"
        if pf.exists():
            # in_domain_queries() evaluates the first 2 probe lines, so keep
            # those OUT of calibration; pass the remainder only if enough is left
            plines = [l.strip() for l in pf.read_text().splitlines() if l.strip()]
            rest = plines[2:]
            if len(rest) >= 3:
                tp = trim / f"{name}_probes.txt"
                tp.write_text("\n".join(rest) + "\n")
                cmd += ["--probes", str(tp)]
        r = sp.run(cmd, capture_output=True, text=True)
        if r.returncode == 0 and (out / f"{name}.gencap").exists():
            sealed.append(name)
        else:
            refused.append(name)
        sys.stderr.write(f"\r  reseal {i}/{len(names)} sealed={len(sealed)} refused={len(refused)}")
    sys.stderr.write("\n")
    return sealed, refused, missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="bin")
    ap.add_argument("--reseal-heldout", default="",
                    help="directory to reseal capsules into from corpora minus the held-out "
                         "sentences; the eval then runs on that directory so in-domain queries "
                         "are genuinely held out (default: evaluate --dir as is)")
    ap.add_argument("--encoder", default="default", help="encoder for --reseal-heldout")
    ap.add_argument("--target-in", type=float, default=0.80)
    ap.add_argument("--target-neg", type=float, default=0.90)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--skip", type=int, default=0, help="skip the first N capsules (disjoint test slices)")
    ap.add_argument("--probes-dir", default="var/distill")
    ap.add_argument("--out", default="")
    ap.add_argument("--allow-name-queries", action="store_true")
    ap.add_argument("--ood-extra", type=int, default=0,
                    help="extra OOD sentences from distill corpora with no matching .gencap "
                         "(0 default: those sentences are sibling topics, not aliens)")
    args = ap.parse_args()

    reseal_note = ""
    if args.reseal_heldout:
        sealed, refused, missing = reseal_heldout(args.dir, args.probes_dir, args.reseal_heldout,
                                                  args.encoder, args.target_in, args.target_neg, args.limit, args.skip)
        reseal_note = (f"Resealed from corpora minus last {HOLDOUT} sentences (encoder={args.encoder}, "
                       f"targets {args.target_in}/{args.target_neg}, negatives from a globally trimmed copy "
                       f"of every corpus so no held-out line is a negative anywhere): sealed={len(sealed)} "
                       f"refused_by_calibration={len(refused)} no_corpus={len(missing)}.")
        args.dir = args.reseal_heldout
        args.limit = 0

    all_caps = sorted(p.stem for p in Path(args.dir).glob("*.gencap"))
    caps = all_caps[args.skip:] if (args.skip and not args.reseal_heldout) else all_caps
    caps = caps[: args.limit] if (args.limit and not args.reseal_heldout) else caps
    cap_set = set(all_caps)
    name_tokens = set()
    for n in all_caps:
        name_tokens |= content_tokens(n)

    skipped = 0
    in_rows = []
    src_counts = {"probes": 0, "corpus": 0, "name": 0}
    pending = []  # (capsule, query, src)
    for name in caps:
        queries, src = in_domain_queries(name, args.probes_dir, args.allow_name_queries)
        if not queries:
            skipped += 1
            continue
        src_counts[src] = src_counts.get(src, 0) + len(queries)
        for q in queries:
            pending.append((name, q, src))
    sys.stderr.write(f"  routing {len(pending)} in-domain queries over {len(caps)} capsules (one registry load)\n")
    for (name, q, src), r in zip(pending, route_many(args.dir, [p[1] for p in pending])):
        if r:
            r.update(capsule=name, query=q, src=src)
            in_rows.append(r)

    dropped_aliens = []
    alien_keep = []
    for q in ALIEN:
        overlap = {t for t in (content_tokens(q) & name_tokens) if t not in GENERIC_NAME and len(t) >= 6}
        if overlap:
            dropped_aliens.append((q, sorted(overlap)[:6]))
        else:
            alien_keep.append(q)

    extra_ood = []
    distill = Path(args.probes_dir)
    if distill.is_dir() and args.ood_extra > 0:
        for cf in sorted(distill.glob("*_corpus.txt")):
            stem = cf.name[: -len("_corpus.txt")]
            if stem in cap_set:
                continue
            for line in cf.read_text().splitlines():
                line = line.strip()
                if len(line) < 40:
                    continue
                if distinctive_overlap(line, " ".join(sorted(name_tokens))):
                    continue
                extra_ood.append(line)
                break
            if len(extra_ood) >= args.ood_extra:
                break

    ood_queries = alien_keep + extra_ood
    alien_rows = []
    for q, r in zip(ood_queries, route_many(args.dir, ood_queries)):
        if not r:
            continue
        overlap = distinctive_overlap(q, r["top"])
        if overlap:
            dropped_aliens.append((q, sorted(overlap)[:6]))
            continue
        r.update(query=q, src="ood")
        alien_rows.append(r)

    def rate(rows, key):
        return sum(1 for r in rows if r[key]) / len(rows) if rows else float("nan")

    for r in in_rows:
        r["legacy_correct"] = r["legacy_accept"] and r["top"] == r["capsule"]
        r["new_correct"] = r["new_accept"] and r["top"] == r["capsule"]
        r["top_is_self"] = r["top"] == r["capsule"]
        r["legacy_wrong"] = r["legacy_accept"] and r["top"] != r["capsule"]
        r["new_wrong"] = r["new_accept"] and r["top"] != r["capsule"]
        r["legacy_abstain"] = not r["legacy_accept"]
        r["new_abstain"] = not r["new_accept"]

    def score(rows, correct, wrong):
        if not rows:
            return float("nan")
        return sum(1 for r in rows if r[correct]) - WRONG_COST * sum(1 for r in rows if r[wrong])

    lines = []
    spaces = sorted({r["space"] for r in in_rows + alien_rows})
    lines.append(f"# VSA fail-closed arena eval ({args.dir}, {len(caps)} capsules)\n")
    lines.append(f"Routing space(s) observed: {', '.join(spaces)}"
                 + ("  <-- WARNING: reseal requested but not every query routed in the wide space"
                    if args.reseal_heldout and spaces != ["wide"] else ""))
    lines.append("Scoring: `score = n_correct - 2 * n_wrong_accept`. Abstain = 0.")
    lines.append("In-domain gold = the capsule the query was drawn from. OOD gold = abstain.")
    if reseal_note:
        lines.append(reseal_note)
    else:
        lines.append("NOTE: corpus-source queries were inside the centroid at seal time unless "
                     "--reseal-heldout was used; treat their accept rate as optimistic.")
    lines.append(
        f"Query sources: probes={src_counts['probes']} corpus={src_counts['corpus']} "
        f"name={src_counts['name']} skipped_no_text={skipped}."
    )
    lines.append(
        f"OOD: kept {len(alien_keep)}/{len(ALIEN)} hand aliens "
        f"(dropped {len(dropped_aliens)} as covered-topic), "
        f"plus {len(extra_ood)} sentences from uncovered corpora.\n"
    )
    lines.append("| set | n | src | legacy accept | new accept | correct | wrong | abstain | top-1 self | new score |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|")
    src_note = "probes/corpus" + ("/name" if args.allow_name_queries else "")
    lines.append(
        f"| in-domain | {len(in_rows)} | {src_note} | {rate(in_rows,'legacy_accept'):.3f} | "
        f"{rate(in_rows,'new_accept'):.3f} | {rate(in_rows,'new_correct'):.3f} | "
        f"{rate(in_rows,'new_wrong'):.3f} | {rate(in_rows,'new_abstain'):.3f} | "
        f"{rate(in_rows,'top_is_self'):.3f} | {score(in_rows,'new_correct','new_wrong'):.0f} |"
    )
    ood_new_wrong = rate(alien_rows, "new_accept")  # any OOD accept is wrong
    ood_new_abstain = 1.0 - ood_new_wrong if alien_rows else float("nan")
    ood_score = 0 - WRONG_COST * sum(1 for r in alien_rows if r["new_accept"]) if alien_rows else float("nan")
    lines.append(
        f"| ood | {len(alien_rows)} | alien+uncovered | {rate(alien_rows,'legacy_accept'):.3f} | "
        f"{rate(alien_rows,'new_accept'):.3f} | 0.000 | {ood_new_wrong:.3f} | "
        f"{ood_new_abstain:.3f} | - | {ood_score:.0f} |"
    )
    lines.append("")
    if dropped_aliens:
        lines.append("## Covered-topic queries removed from the alien set")
        lines.append("| query | overlapping name tokens |")
        lines.append("|---|---|")
        for q, ov in dropped_aliens:
            lines.append(f"| {q[:70]} | {', '.join(ov)} |")
        lines.append("")

    zs = [r["z"] for r in in_rows if r["z"] == r["z"]]
    za = [r["z"] for r in alien_rows if r["z"] == r["z"]]
    if zs and za:
        lines.append(
            f"z-score: in-domain median {sorted(zs)[len(zs)//2]:.2f}, "
            f"ood max {max(za):.2f}, required {in_rows[0]['zmin']:.2f}\n"
        )

    lines.append("## OOD queries")
    lines.append("| query | closest | dist | z | legacy | new |")
    lines.append("|---|---|---|---|---|---|")
    for r in alien_rows[:40]:
        lines.append(
            f"| {r['query'][:60]} | {r['top']} | {r['dist']:.3f} | {r['z']:.2f} | "
            f"{'ACCEPT' if r['legacy_accept'] else 'refuse'} | "
            f"{'ACCEPT' if r['new_accept'] else 'refuse'} |"
        )
    lines.append("")
    lost = [r for r in in_rows if r["legacy_correct"] and not r["new_correct"]]
    lines.append(f"## In-domain queries lost to the margin gate ({len(lost)})")
    lines.append("| capsule | src | query | z | z_min | dist |")
    lines.append("|---|---|---|---|---|---|")
    for r in lost[:40]:
        lines.append(
            f"| {r['capsule']} | {r['src']} | {r['query'][:50]} | {r['z']:.2f} | {r['zmin']:.2f} | {r['dist']:.3f} |"
        )
    lines.append("")
    lines.append("WITHHELD: generated-text quality; transformer baseline (see cnet_vsa_vs_transformer.py).")
    text = "\n".join(lines) + "\n"
    print(text)
    if args.out:
        Path(args.out).write_text(text)


if __name__ == "__main__":
    main()
