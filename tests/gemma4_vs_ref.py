#!/usr/bin/env python3
"""gemma4 forward validation: CNET vs llama.cpp on the same GGUF.

For each prompt: tokenize via the reference server, run greedy generation on
both engines, gate on ARGMAX IDENTITY at every step. Softcap note: the
reference caps logits (30*tanh(l/30)); CNET ranks uncapped — monotone, so
argmax agrees except at fp32 saturation ties (reported, not hidden).

Usage: tests/gemma4_vs_ref.py [--server http://127.0.0.1:8083] [--steps 16]
Requires: llama-server running on the SAME GGUF; bin/gemma4_vs_ref built.
"""
import json, os, re, subprocess, sys, urllib.request

SERVER = "http://127.0.0.1:8083"
MODEL  = "/home/marble/AI/Models/gemma4-v2-Q4_K_M.gguf"
STEPS  = 16
args = sys.argv[1:]
if "--server" in args: SERVER = args[args.index("--server")+1]
if "--steps"  in args: STEPS  = int(args[args.index("--steps")+1])

PROMPTS = [
    "The capital of France is",
    "2 + 2 =",
    "The chemical symbol for gold is",
    "Roses are red, violets are",
    "def fibonacci(n):",
    "The Eiffel Tower is located in the city of",
]

def post(path, body):
    req = urllib.request.Request(SERVER + path, json.dumps(body).encode(),
                                 {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=300) as r:
        return json.load(r)

def ref_chain(ids, steps):
    # natural EOG stop — no ignore_eos: forcing past end-of-turn suppresses
    # the true argmax and unfairly diverges from a faithful engine
    r = post("/completion", {"prompt": ids, "n_predict": steps,
                             "temperature": 0.0, "n_probs": 2})
    toks = [t["id"] for t in r.get("completion_probabilities", [])][:steps]
    gaps = []
    for t in r.get("completion_probabilities", [])[:steps]:
        tl = t.get("top_logprobs") or t.get("probs") or []
        gaps.append(round(tl[0].get("logprob", 0) - tl[1].get("logprob", 0), 4)
                    if len(tl) >= 2 else None)
    return toks, gaps

def cnet_chain(ids, steps):
    out = subprocess.run(
        ["./bin/gemma4_vs_ref", MODEL, ",".join(map(str, ids)), str(steps), "3"],
        capture_output=True, text=True, timeout=3600)
    if out.returncode != 0:
        print(out.stderr[-400:], file=sys.stderr)
        raise RuntimeError("cnet driver failed")
    chain = [int(m.group(1)) for m in
             re.finditer(r"^STEP\s+\d+ next=(\d+)", out.stdout, re.M)]
    return chain[:steps]

only = os.environ.get("GVR_ONLY")
if only:
    keys = [k.strip().lower() for k in only.split(",")]
    PROMPTS = [p for p in PROMPTS if any(k in p.lower() for k in keys)]

fails = 0
for p in PROMPTS:
    ids = post("/tokenize", {"content": p, "add_special": True})["tokens"]
    ref, gaps = ref_chain(ids, STEPS)
    got = cnet_chain(ids, STEPS)
    # compare up to the reference's natural stop (EOG ends its chain)
    n = min(len(ref), len(got))
    div = next((i for i in range(n) if ref[i] != got[i]), None)
    ok = div is None and n > 0
    fails += (not ok)
    tag = "IDENTICAL" if ok else f"DIVERGES@{div}" if div is not None else "EMPTY"
    print(f"{tag:12s} steps={n:3d}  {p!r}")
    if not ok and div is not None:
        print(f"    ref : {ref[:div+3]}   ref top1-top2 gap at divergence: {gaps[div]}")
        print(f"    cnet: {got[:div+3]}")

print(f"\n{'GATE PASSED' if fails == 0 else 'GATE FAILED'}: "
      f"{len(PROMPTS)-fails}/{len(PROMPTS)} prompts argmax-identical x {STEPS} steps")
sys.exit(1 if fails else 0)
