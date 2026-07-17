#!/usr/bin/env python3
"""Generate a window-screen candidate pool from a model's OWN vocabulary.

In byte-level/SentencePiece BPE the token-id order is the merge order,
which is the training corpus's frequency order — so "the model's N most
frequent space-prefixed english word tokens" is exactly the candidate
pool CNET_WINDOW_SCREEN wants, with provenance = the model artifact
itself (no external wordlist).

Selection rule: a vocab piece qualifies when its normalized surface form
is a leading space + 2..14 lowercase ascii letters (the english_window
convention — one-hot units mine word-boundary tokens, never subwords
glued mid-word). Ids are emitted in ascending order (frequency rank).

BPE merge order also promotes NON-word fragments early (' th', ' re'),
so qualifying surfaces are additionally checked against a dictionary
(default /usr/share/dict/words) — a fragment in the pool would let the
screen mint a unit for a non-word.

Usage:
  gen_window_candidates.py <model.gguf> <count> -o <out.txt>
Writes <out.txt> (one decimal id per line, CNET_WINDOW_SCREEN /
CNET_WINDOW_FILE format) and <out.txt>.words.txt (id<TAB>word, with
provenance header comments — the id file stays bare for fscanf).
"""
import argparse
import hashlib
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xlate_window import read_gguf_bpe, normalize

WORD = re.compile(r"^ [a-z]{2,14}$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("count", type=int)
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--dict", default="/usr/share/dict/words",
                    help="wordlist; one word per line (default: %(default)s)")
    a = ap.parse_args()

    if not os.path.exists(a.dict):
        sys.exit("dictionary %s missing — pass --dict (refusing to emit "
                 "unchecked BPE fragments)" % a.dict)
    # only entries that are ALREADY all-lowercase in the dictionary count:
    # case-folding would smuggle in abbreviations and proper nouns
    # ("Th" thorium, "St" saint) as fake lowercase words
    dict_words = set(w.strip() for w in open(a.dict, errors="replace")
                     if w.strip().isalpha() and w.strip().islower())
    dict_sha = hashlib.sha256(open(a.dict, "rb").read()).hexdigest()

    tokens, _merges, _ttype = read_gguf_bpe(a.model)
    picked = []
    seen_words = set()
    for tid, piece in enumerate(tokens):
        s = normalize(piece)
        if not WORD.match(s):
            continue
        w = s[1:]
        if w not in dict_words:
            continue
        if w in seen_words:   # a vocab can carry duplicate surface forms
            continue
        seen_words.add(w)
        picked.append((tid, w))
        if len(picked) >= a.count:
            break
    if len(picked) < a.count:
        sys.exit("only %d qualifying word tokens in %s (wanted %d)"
                 % (len(picked), a.model, a.count))

    with open(a.out, "w") as f:
        for tid, _w in picked:
            f.write("%d\n" % tid)
    with open(a.out + ".words.txt", "w") as f:
        f.write("# provenance: model=%s\n" % a.model)
        f.write("# dict=%s sha256=%s\n" % (a.dict, dict_sha))
        f.write("# rule: ascending token id (BPE frequency rank), "
                "surface ' [a-z]{2,14}', dictionary-checked, deduped\n")
        for tid, w in picked:
            f.write("%d\t%s\n" % (tid, w))

    h = hashlib.sha256(open(a.out, "rb").read()).hexdigest()
    print("wrote %d candidates to %s (sha256 %s)" % (len(picked), a.out, h))
    print("first: %s" % ", ".join("%d=%s" % p for p in picked[:6]))


if __name__ == "__main__":
    main()
