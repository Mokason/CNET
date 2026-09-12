#!/usr/bin/env python3
"""Covered-domain QA: CNET capsules vs a transformer, same questions, same right to abstain.

CNET side: route (margin+radius) then retrieve the nearest sealed teacher sentence
from var/distill/<winner>_corpus.txt. That is hyperdimensional cleanup, not n-gram
generation and not a transformer.

Transformer side: POST an OpenAI-style chat completion. The model is instructed
that it may answer ABSTAIN. If no endpoint is reachable, the transformer lane is
WITHHELD — this script will not invent a score and will not use the 27B teacher
that wrote the gold (that comparison is circular).

Score: correct - 2 * wrong. Abstain = 0.
Correct CNET: routed to the gold capsule AND retrieved sentence shares >= 3
content tokens with the gold sentence.
Correct transformer: answer is not ABSTAIN and shares >= 3 content tokens with gold.
Wrong: answered, and failed the token overlap (or CNET routed to the wrong capsule).

Usage:
  tools/cnet_vsa_vs_transformer.py --dir bin --limit 40
  tools/cnet_vsa_vs_transformer.py --transformer-url http://127.0.0.1:8081/v1/chat/completions
"""
import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cnet_vsa_route_eval import (  # noqa: E402
    WRONG_COST,
    content_tokens,
    in_domain_queries,
    route,
)


def retrieve(corpus_path: Path, query: str) -> str:
    """Nearest sealed sentence by overlapping content tokens.

    A true HD retrieve lives in the C arena (codebook cleanup). This fallback is
    lexical overlap over certified teacher sentences so the live script does not
    need to re-encode 800 corpora in Python.
    """
    if not corpus_path.exists():
        return ""
    q = content_tokens(query)
    best, best_n = "", -1
    for line in corpus_path.read_text().splitlines():
        line = line.strip()
        if len(line) < 20:
            continue
        n = len(q & content_tokens(line))
        if n > best_n:
            best, best_n = line, n
    return best


def transformer_qa(url: str, question: str, timeout: float = 20.0) -> str | None:
    body = json.dumps({
        "model": os.environ.get("CNET_VS_TRANSFORMER_MODEL", "local"),
        "temperature": 0.0,
        "max_tokens": 128,
        "messages": [
            {
                "role": "system",
                "content": (
                    "Answer the factual question in one or two sentences. "
                    "If you are not sure the question is in your knowledge, reply with exactly ABSTAIN."
                ),
            },
            {"role": "user", "content": question},
        ],
    }).encode()
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode())
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, ValueError):
        return None
    try:
        return data["choices"][0]["message"]["content"].strip()
    except (KeyError, IndexError, TypeError, AttributeError):
        return None


def overlap_ok(a: str, b: str, n: int = 3) -> bool:
    return len(content_tokens(a) & content_tokens(b)) >= n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="bin")
    ap.add_argument("--probes-dir", default="var/distill")
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--transformer-url", default=os.environ.get("CNET_VS_TRANSFORMER_URL", ""))
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    caps = sorted(p.stem for p in Path(args.dir).glob("*.gencap"))
    if args.limit:
        caps = caps[: args.limit]

    rows = []
    for name in caps:
        qs, src = in_domain_queries(name, args.probes_dir, allow_name=False)
        if not qs:
            continue
        gold_q = qs[0]
        gold_text = gold_q

        r = route(args.dir, gold_q)
        if not r:
            continue
        cnet_ans = ""
        cnet_kind = "abstain"
        if r["new_accept"]:
            cnet_kind = "correct" if r["top"] == name else "wrong"
            cnet_ans = retrieve(Path(args.probes_dir) / f"{r['top']}_corpus.txt", gold_q)
        rows.append({
            "capsule": name, "src": src, "query": gold_q, "gold": gold_text,
            "cnet_top": r["top"], "cnet_accept": r["new_accept"],
            "cnet_kind": cnet_kind, "cnet_ans": cnet_ans,
        })
        sys.stderr.write(f"\r  cnet {len(rows)}/{len(caps)}")
    sys.stderr.write("\n")

    tf_url = args.transformer_url.strip()
    tf_ok = False
    if tf_url:
        probe = transformer_qa(tf_url, "Reply with ABSTAIN.")
        tf_ok = probe is not None
        if tf_ok:
            for row in rows:
                ans = transformer_qa(tf_url, row["query"])
                if ans is None:
                    tf_ok = False
                    break
                if ans.upper().startswith("ABSTAIN"):
                    row["tf_kind"] = "abstain"
                    row["tf_ans"] = "ABSTAIN"
                elif overlap_ok(ans, row["gold"]):
                    row["tf_kind"] = "correct"
                    row["tf_ans"] = ans
                else:
                    row["tf_kind"] = "wrong"
                    row["tf_ans"] = ans
                sys.stderr.write(f"\r  transformer {rows.index(row)+1}/{len(rows)}")
            sys.stderr.write("\n")

    def tally(key):
        c = sum(1 for r in rows if r.get(key) == "correct")
        w = sum(1 for r in rows if r.get(key) == "wrong")
        a = sum(1 for r in rows if r.get(key) == "abstain")
        return c, w, a, c - WRONG_COST * w

    cc, cw, ca, cs = tally("cnet_kind")
    lines = []
    lines.append(f"# CNET vs transformer (fail-closed QA), {len(rows)} questions\n")
    lines.append("Score = correct - 2 * wrong. Abstain = 0. Gold = held-out probe or corpus sentence.")
    lines.append("CNET answer = nearest sealed teacher sentence in the routed capsule, not n-gram generation.\n")
    lines.append("| system | n | correct | wrong | abstain | score |")
    lines.append("|---|---|---|---|---|---|")
    lines.append(f"| CNET capsules (route + retrieve) | {len(rows)} | {cc} | {cw} | {ca} | {cs} |")
    if tf_ok:
        tc, tw, ta, ts = tally("tf_kind")
        lines.append(f"| transformer ({tf_url}) | {len(rows)} | {tc} | {tw} | {ta} | {ts} |")
        if cs > ts:
            lines.append("\nCNET score is higher on this slice. Not a general language win.")
        else:
            lines.append("\nTransformer score is higher or tied on this slice. CNET has not beaten it here.")
    else:
        why = "no --transformer-url / CNET_VS_TRANSFORMER_URL" if not tf_url else f"endpoint not reachable ({tf_url})"
        lines.append(f"| transformer | - | - | - | - | WITHHELD ({why}) |")
        lines.append("\nWITHHELD: transformer lane. Mouth :8084 is a story generator, not a QA endpoint,")
        lines.append("and the 27B teacher that wrote the gold is not a legal baseline.")
    lines.append("\nWITHHELD: open-ended language; energy; multi-hop composition.")
    text = "\n".join(lines) + "\n"
    print(text)
    if args.out:
        Path(args.out).write_text(text)


if __name__ == "__main__":
    main()
