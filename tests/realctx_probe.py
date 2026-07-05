#!/usr/bin/env python3
"""Real-context probe: does gemma4-v2 complete REAL prompts (not bare <bos>)
when we encode them via the model's OWN vocab? Builds prompts from single-token
words + the gemma chat template, runs them through bin/tok_forward, decodes the
top predictions. If this works, real-context extraction is viable.

Usage: python3 tests/realctx_probe.py <model.gguf>
"""
import struct, subprocess, sys, os

MODEL = sys.argv[1] if len(sys.argv) > 1 else "/home/marble/AI/Models/gemma4-v2-Q4_K_M.gguf"

def gguf_tokens(path):
    f = open(path, "rb")
    _, _, _, nkv = struct.unpack("<IIQQ", f.read(24))
    SZ = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,10:8,11:8,12:8}
    def rs():
        n, = struct.unpack("<Q", f.read(8)); return f.read(n)
    toks = None
    for _ in range(nkv):
        k = rs().decode("utf-8","replace"); t, = struct.unpack("<I", f.read(4))
        if t == 8: rs()
        elif t == 9:
            et, n = struct.unpack("<IQ", f.read(12))
            if k == "tokenizer.ggml.tokens" and et == 8:
                toks = [rs().decode("utf-8","replace") for _ in range(n)]; break
            elif et == 8:
                for _ in range(n): rs()
            else: f.read(SZ[et]*n)
        else: f.read(SZ[t])
    return toks

toks = gguf_tokens(MODEL)
str2id = {}
for i, s in enumerate(toks):
    str2id.setdefault(s, i)
def tid(s):                       # gemma marks a word start with U+2581
    return str2id.get("▁"+s) or str2id.get(s)
def dec(i):
    return toks[i].replace("▁"," ") if 0 <= i < len(toks) else f"<{i}>"

# gemma chat/control tokens
BOS = str2id.get("<bos>")
SOT = str2id.get("<start_of_turn>")
EOT = str2id.get("<end_of_turn>")
NL  = tid("\n") or str2id.get("\n")
print(f"specials: bos={BOS} start_of_turn={SOT} end_of_turn={EOT} nl={NL}")

def words(ids): return "|".join(dec(i) for i in ids)

prompts = [
    ("The capital of France is",   ["The","capital","of","France","is"]),
    ("The capital of Japan is",    ["The","capital","of","Japan","is"]),
    ("Two plus two equals",        ["Two","plus","two","equals"]),
    ("The opposite of hot is",     ["The","opposite","of","hot","is"]),
    ("The sky is",                 ["The","sky","is"]),
    ("Water is made of hydrogen and", ["Water","is","made","of","hydrogen","and"]),
]

# build every prompt's token line (plain + chat-templated), run in ONE model load
jobs = []   # (label, mode, token_ids)
for text, ws in prompts:
    plain = [BOS] + [tid(w) for w in ws]
    jobs.append((text, "plain", plain))
    if SOT and EOT and NL:
        chat = [BOS, SOT, tid("user"), NL] + [tid(w) for w in ws] + [EOT, NL, SOT, tid("model"), NL]
        jobs.append((text, "chat", chat))

stdin = "\n".join(" ".join(str(x) for x in (ids if None not in ids else [BOS]))
                  for _, _, ids in jobs) + "\n"
proc = subprocess.run(["bin/tok_forward", MODEL], input=stdin, capture_output=True, text=True,
                      env={**os.environ, "CNET_ORACLE_INT8":"1", "CNET_GPU":"1"})
tops = [l for l in proc.stdout.splitlines() if l.startswith("TOP") or l == "ERR"]

print("\n=== real-context completions (top-5 next tokens) ===")
for (text, mode, ids), line in zip(jobs, tops):
    if None in ids:
        print(f"  [{mode:5s}] {text!r:38s} -> (untokenizable: {words([i for i in ids if i is not None])})"); continue
    top = [int(x) for x in line.split()[1:]][:5] if line.startswith("TOP") else []
    print(f"  [{mode:5s}] {text!r:38s} -> {words(top)}")
