#!/usr/bin/env python3
"""Gate for the frozen routing arena.

1. Verifies the fixture files against manifest.json (sha256).
2. Loads results_cnet.json (from tools/cnet_vsa_arena.c) and every
   results_<model>.json produced by tools/cnet_vsa_arena_transformer.py.
3. Prints one table, and compares every number against expected.json within
   the tolerances recorded there. A missing expected.json is written from the
   current results (first freeze) and the gate reports FROZEN, not PASS.

Marker: CNET_VSA_ROUTING_ARENA_PASS
"""
import hashlib, json, sys
from pathlib import Path

fdir = Path(sys.argv[1] if len(sys.argv) > 1 else "benchmarks/vsa_routing_arena_20260911")
man = json.loads((fdir / "manifest.json").read_text())

fails = []
for fname, want in man["sha256"].items():
    got = hashlib.sha256((fdir / fname).read_bytes()).hexdigest()
    if got != want:
        fails.append(f"fixture {fname} sha256 {got[:16]} != manifest {want[:16]}")
print(f"fixture: {fdir} ({', '.join(man['sha256'])}) sha256 {'OK' if not fails else 'MISMATCH'}")

cnet = json.loads((fdir / "results_cnet.json").read_text())
rows = []
for enc, r in cnet["encoders"].items():
    rows.append((f"cnet {enc}", r))
for p in sorted(fdir.glob("results_*.json")):
    if p.name == "results_cnet.json":
        continue
    r = json.loads(p.read_text())
    rows.append((f"transformer {r['model']}", r))


def g(r, *keys, default=float("nan")):
    x = r
    for k in keys:
        if not isinstance(x, dict) or k not in x:
            return default
        x = x[k]
    return x


print("\n| system | test top-1 | test top-3 | questions top-1 | questions top-3 | separable 0.80/0.90 | separable 0.90/0.95 | alien z | encode |")
print("|---|---|---|---|---|---|---|---|---|")
for name, r in rows:
    enc_us = g(r, "encode_us", default=None)
    if enc_us is None:
        t = g(r, "timings_s", "train_embed_s", default=None)
        n = None
        enc = "cached" if t is None else f"{1e6 * t / max(1, r.get('corpora', 1) * 57):.0f} us*"
    else:
        enc = f"{enc_us:.1f} us"
    print(f"| {name} | {100*g(r,'test_sentences','top1'):.1f}% | {100*g(r,'test_sentences','top3'):.1f}% | "
          f"{100*g(r,'questions','top1'):.1f}% | {100*g(r,'questions','top3'):.1f}% | "
          f"{100*g(r,'separability','0.80/0.90','fraction'):.1f}% | {100*g(r,'separability','0.90/0.95','fraction'):.1f}% | "
          f"{g(r,'aliens','z_mean'):.2f} | {enc} |")

# post-freeze question sets (written after frozen_model.json): reported, not frozen
extra = [(name, k, r[k]) for name, r in rows for k in sorted(r) if k.startswith("questions_extra") and isinstance(r[k], dict)]
if extra:
    print("\n| system | post-freeze set | top-1 | top-1 lenient (alternates) | top-3 | n |")
    print("|---|---|---|---|---|---|")
    for name, k, x in extra:
        print(f"| {name} | {Path(x.get('source', k)).name} | {100*x['top1']:.1f}% | {100*x.get('top1_lenient', float('nan')):.1f}% | {100*x['top3']:.1f}% | {x['n']} |")

# the model is frozen: a rebuilt table with a different digest is a different model
fz_path = fdir / "frozen_model.json"
if fz_path.exists():
    fz = json.loads(fz_path.read_text())
    for key in ("lexicon", "lexicon_distilled", "lexicon_full", "lexicon_trained"):
        want = fz.get(key, {}).get("digest")
        got = cnet.get(key, {}).get("digest")
        if want and got and want != got:
            fails.append(f"frozen model changed: {key} digest {got} != {want} (frozen {fz.get('frozen_at')})")
    print(f"\nfrozen model ({fz.get('frozen_at')}): " + ("digests match" if not any(f.startswith("frozen model") for f in fails) else "DIGEST MISMATCH"))

# expected values and tolerances
exp_path = fdir / "expected.json"
refreeze = "--refreeze" in sys.argv[2:]   # rewrite the frozen values, keep claims and notes (deliberate, logged)
flat = {}
for name, r in rows:
    for key, val in (("test_top1", g(r, "test_sentences", "top1")), ("test_top3", g(r, "test_sentences", "top3")),
                     ("q_top1", g(r, "questions", "top1")), ("q_top3", g(r, "questions", "top3")),
                     ("sep_80_90", g(r, "separability", "0.80/0.90", "fraction")),
                     ("sep_90_95", g(r, "separability", "0.90/0.95", "fraction"))):
        if val == val:  # not nan
            flat[f"{name}::{key}"] = val
    for k in sorted(r):
        if k.startswith("questions_extra:") and isinstance(r[k], dict):
            base = k.split(":", 1)[1]
            for key in ("top1", "top3", "top1_lenient"):
                if key in r[k]:
                    flat[f"{name}::{base}::{key}"] = r[k][key]
if not exp_path.exists():
    exp_path.write_text(json.dumps({"tolerance": 0.01, "values": flat}, indent=2) + "\n")
    print(f"\nexpected.json written with {len(flat)} values (tolerance 0.01). CNET_VSA_ROUTING_ARENA_FROZEN")
    sys.exit(0)
exp = json.loads(exp_path.read_text())
if refreeze:
    core = {k: v for k, v in flat.items() if k.count("::") == 1}   # extra sets are floors, not two-sided values
    changed = [k for k, v in core.items() if abs(exp["values"].get(k, float("nan")) - v) > exp.get("tolerance", 0.01)] + \
              [k for k in core if k not in exp["values"]]
    exp["values"] = core
    if "floors" in exp:
        exp["floors"] = {k: flat[k] for k in exp["floors"] if k in flat}
    exp_path.write_text(json.dumps(exp, indent=2) + "\n")
    print(f"\nexpected.json refrozen: {len(core)} values, {len(changed)} new/changed: {', '.join(changed) or '-'}. CNET_VSA_ROUTING_ARENA_FROZEN")
    sys.exit(0)
tol = exp.get("tolerance", 0.01)
for k, want in exp["values"].items():
    got = flat.get(k)
    if got is None:
        fails.append(f"missing {k}")
    elif abs(got - want) > tol:
        fails.append(f"{k}: {got:.4f} vs expected {want:.4f} (tol {tol})")
# regression floors: one-sided, a value may rise but not fall below its recorded level
for k, want in exp.get("floors", {}).items():
    got = flat.get(k)
    if got is None:
        fails.append(f"missing floor value {k}")
    elif got + tol < want:
        fails.append(f"regression: {k} {got:.4f} fell below floor {want:.4f}")
    else:
        print(f"  floor ok {k}: {got:.3f} >= {want:.3f}")

# The claims this arena exists to keep honest, as explicit orderings recorded in
# expected.json ("claims": [{"lhs", "rhs", "key", "label"}]). A claim that stops
# holding fails the gate; a claim that is not listed is not made.
for c in exp.get("claims", []):
    a, b = flat.get(f"{c['lhs']}::{c['key']}"), flat.get(f"{c['rhs']}::{c['key']}")
    if a is None or b is None:
        fails.append(f"claim needs missing value: {c}")
        continue
    ok = a + tol >= b
    print(("  holds   " if ok else "  FAILS   ") + f"{c['label']}: {c['lhs']} {a:.3f} vs {c['rhs']} {b:.3f}")
    if not ok:
        fails.append("claim: " + c["label"])

if fails:
    print("\nCNET_VSA_ROUTING_ARENA_FAIL")
    for f in fails:
        print("  " + f)
    sys.exit(1)
print(f"\nCNET_VSA_ROUTING_ARENA_PASS: {len(exp['values'])} frozen values within {tol}, fixture hashes verified, {len(exp.get('claims', []))} claims hold")
