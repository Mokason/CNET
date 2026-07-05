#!/usr/bin/env python3
"""Decode soul_query output to text using the model's OWN embedded gemma4
vocab (the repo tokenizer.json is a different, GPT-2-style tokenizer — do not
use it for gemma ids). Reads the GGUF's tokenizer.ggml.tokens directly.

Usage: bin/soul_query <model.gguf> <base.cnb> | python3 tests/soul_query.py <model.gguf>
"""
import struct, sys


def gguf_tokens(path):
    f = open(path, "rb")
    _, _, _, nkv = struct.unpack("<IIQQ", f.read(24))
    SZ = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 4, 7: 1, 10: 8, 11: 8, 12: 8}

    def rs():
        n, = struct.unpack("<Q", f.read(8))
        return f.read(n)

    for _ in range(nkv):
        k = rs().decode("utf-8", "replace")
        t, = struct.unpack("<I", f.read(4))
        if t == 8:
            rs()
        elif t == 9:
            et, n = struct.unpack("<IQ", f.read(12))
            if k == "tokenizer.ggml.tokens" and et == 8:
                return [rs().decode("utf-8", "replace") for _ in range(n)]
            elif et == 8:
                for _ in range(n):
                    rs()
            else:
                f.read(SZ[et] * n)
        else:
            f.read(SZ[t])
    return None


def main():
    toks = gguf_tokens(sys.argv[1])

    def d(i):
        i = int(i)
        if i < 0:
            return "."
        s = toks[i] if i < len(toks) else "<%d>" % i
        return s.replace("▁", " ").strip() or "_"

    lines = sys.stdin.read().splitlines()
    win = [d(x) for x in lines[0].split()[1:]]
    print("Window (gemma4's top continuations of bare <bos>, first 16):")
    print("  " + " | ".join(win[:16]))
    print()
    m = t = 0
    rows = []
    for ln in lines:
        if not ln.startswith("Q "):
            continue
        p = ln.split()
        ok = p[4:7] == p[8:11]
        rows.append((ok, d(p[1]), d(p[2]), [d(x) for x in p[4:7]],
                     [d(x) for x in p[8:11]]))
        t += 1
        m += ok
    shown = 0
    for ok, A, B, soul, orc in rows:
        if ok and shown < 6:
            print("  OK  after [%s][%s]  ->  %s" % (A, B, " / ".join(soul)))
            shown += 1
    for ok, A, B, soul, orc in rows:
        if not ok:
            print("  XX  after [%s][%s]  ->  soul: %s  |  model: %s"
                  % (A, B, " / ".join(soul), " / ".join(orc)))
    print("\nFIDELITY: soul == live model on %d/%d queries (%d%%)"
          % (m, t, 100 * m // t if t else 0))
    print("(misses are margin-abstained inputs — the model's own near-ties, "
          "uncertified by design.)")


if __name__ == "__main__":
    main()
