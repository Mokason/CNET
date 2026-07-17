#!/usr/bin/env python3
"""Generate DIFFERENT-config transformers for the cce_transformer_qat
load+parity gates, so the pipeline is exercised on models that are NOT Supra.

Two modes (second CLI arg):

  supra (default) -- 4-layer model in Supra's tensor naming with every FREE
    dimension changed vs Supra (D128/V2000/B96/mlp512). Weights follow the
    PyTorch Linear [out, in] convention the real Supra checkpoint uses (the
    loader transposes to the forest's [in, out]). Kept byte-compatible with
    the original generator (same seed/order) -> make transformer_qat_altmodel.

  gpt2 -- the same alt-family model under GPT-2-STYLE tensor naming
    (wte/wpe, h.{i}.ln_1, h.{i}.attn.c_attn, ...) with a DIFFERENT layer
    count (6) and head count (2), Conv1D [in, out] block weights (GPT-2's
    layout -- NO transpose on load), random biases, and a TIED head (no
    lm_head tensor, exactly like the real HF gpt2 checkpoint). n_head rides
    in the safetensors __metadata__. Also embeds GOLDEN logits ("golden.ids"/
    "golden.logits") computed by an independent numpy forward, so the C gate
    can assert semantic load correctness (incl. the Conv1D no-transpose and
    the tied head), not just trainer-vs-decomposer agreement.
    -> make transformer_qat_gpt2names.

  gpt2_nohead / gpt2_gap -- tiny CORRUPT gpt2-named variants for the loader's
    fail-closed gates: no n_head anywhere (loader must refuse, not fall back
    to Supra's 4) / a gap in the layer numbering (loader must refuse, not
    silently truncate). -> make transformer_qat_gpt2names.

Weights are random (small init); the point is the loader/trainer/forward
handling other configs/namings, proven by forward parity, not any learned
behavior.

Usage: python3 tools/gen_altmodel.py [out_dir] [supra|gpt2|gpt2_nohead|gpt2_gap]
"""
import sys, os
import numpy as np
from safetensors.numpy import save_file

out_dir = sys.argv[1] if len(sys.argv) > 1 else "altmodel_cache"
schema = sys.argv[2] if len(sys.argv) > 2 else "supra"
os.makedirs(out_dir, exist_ok=True)
path = os.path.join(out_dir, "model.safetensors")

if schema == "supra":
    # Original alt model, unchanged (same seed + tensor order as before).
    L, D, H, V, B, M = 4, 128, 4, 2000, 96, 512   # n_layer(=4), n_embd, n_head(=4), vocab, block, mlp
    rng = np.random.default_rng(1234)
    def w(*s): return (rng.standard_normal(s).astype(np.float32) * 0.02)
    def z(*s): return np.zeros(s, np.float32)
    def one(*s): return np.ones(s, np.float32)

    t = {"tok_emb.weight": w(V, D), "pos_emb.weight": w(B, D),
         "ln_f.weight": one(D), "ln_f.bias": z(D), "head.weight": w(V, D)}
    for l in range(L):
        t[f"blocks.{l}.ln1.weight"] = one(D); t[f"blocks.{l}.ln1.bias"] = z(D)
        t[f"blocks.{l}.ln2.weight"] = one(D); t[f"blocks.{l}.ln2.bias"] = z(D)
        t[f"blocks.{l}.attn.qkv.weight"]  = w(3 * D, D); t[f"blocks.{l}.attn.qkv.bias"]  = z(3 * D)
        t[f"blocks.{l}.attn.proj.weight"] = w(D, D);     t[f"blocks.{l}.attn.proj.bias"] = z(D)
        t[f"blocks.{l}.mlp.0.weight"] = w(M, D); t[f"blocks.{l}.mlp.0.bias"] = z(M)
        t[f"blocks.{l}.mlp.2.weight"] = w(D, M); t[f"blocks.{l}.mlp.2.bias"] = z(D)

    save_file(t, path)
    print(f"wrote {path}  config L{L} D{D} H{H} V{V} B{B} M{M}  ({len(t)} tensors)")
    sys.exit(0)

if schema in ("gpt2_nohead", "gpt2_gap"):
    # Tiny CORRUPT variants for the loader's fail-closed gates (no goldens:
    # the C gate only asserts that cce_supra_load_decomposed REFUSES them).
    #   gpt2_nohead -- gpt2 naming, NO n_head in __metadata__ (and the out_dir
    #                  carries no config.json): the 4 fallback is Supra-only,
    #                  so the load must refuse, not guess attention geometry.
    #   gpt2_gap    -- gpt2 naming with layer 2 missing (h.0, h.1, h.3): a
    #                  plain consecutive count would silently truncate to 2
    #                  layers, so the load must refuse ("layer numbering gap").
    L, D, V, B, M = 2, 32, 100, 16, 64
    rng = np.random.default_rng(99)
    def w(*s): return (rng.standard_normal(s).astype(np.float32) * 0.02)
    t = {"wte.weight": w(V, D), "wpe.weight": w(B, D),
         "ln_f.weight": w(D), "ln_f.bias": w(D)}
    layers = [0, 1] if schema == "gpt2_nohead" else [0, 1, 3]
    for l in layers:
        t[f"h.{l}.ln_1.weight"] = w(D); t[f"h.{l}.ln_1.bias"] = w(D)
        t[f"h.{l}.ln_2.weight"] = w(D); t[f"h.{l}.ln_2.bias"] = w(D)
        t[f"h.{l}.attn.c_attn.weight"] = w(D, 3 * D); t[f"h.{l}.attn.c_attn.bias"] = w(3 * D)
        t[f"h.{l}.attn.c_proj.weight"] = w(D, D);     t[f"h.{l}.attn.c_proj.bias"] = w(D)
        t[f"h.{l}.mlp.c_fc.weight"]   = w(D, M);      t[f"h.{l}.mlp.c_fc.bias"]   = w(M)
        t[f"h.{l}.mlp.c_proj.weight"] = w(M, D);      t[f"h.{l}.mlp.c_proj.bias"] = w(D)
    # gpt2_gap keeps n_head so the GAP is the only refusal reason under test.
    meta = None if schema == "gpt2_nohead" else {"n_head": "2"}
    save_file(t, path, metadata=meta)
    print(f"wrote {path}  CORRUPT variant '{schema}' layers={layers} "
          f"meta={'none' if meta is None else meta}  ({len(t)} tensors)")
    sys.exit(0)

if schema != "gpt2":
    sys.exit(f"unknown schema '{schema}' (want: supra | gpt2 | gpt2_nohead | gpt2_gap)")

# ---------------- GPT-2-style naming, 6 layers, 2 heads, Conv1D, tied head --------------
L, D, H, V, B, M = 6, 128, 2, 2000, 96, 512
rng = np.random.default_rng(4321)
def w(*s): return (rng.standard_normal(s).astype(np.float32) * 0.02)
def b(*s): return (rng.standard_normal(s).astype(np.float32) * 0.01)  # random biases: load must carry them
def ln(*s): return (1.0 + rng.standard_normal(s).astype(np.float32) * 0.02)

t = {"wte.weight": w(V, D), "wpe.weight": w(B, D),
     "ln_f.weight": ln(D), "ln_f.bias": b(D)}
for l in range(L):
    t[f"h.{l}.ln_1.weight"] = ln(D); t[f"h.{l}.ln_1.bias"] = b(D)
    t[f"h.{l}.ln_2.weight"] = ln(D); t[f"h.{l}.ln_2.bias"] = b(D)
    # Conv1D convention: weight stored [in, out] (y = x @ W + b), NOT torch Linear [out, in]
    t[f"h.{l}.attn.c_attn.weight"] = w(D, 3 * D); t[f"h.{l}.attn.c_attn.bias"] = b(3 * D)
    t[f"h.{l}.attn.c_proj.weight"] = w(D, D);     t[f"h.{l}.attn.c_proj.bias"] = b(D)
    t[f"h.{l}.mlp.c_fc.weight"]   = w(D, M);      t[f"h.{l}.mlp.c_fc.bias"]   = b(M)
    t[f"h.{l}.mlp.c_proj.weight"] = w(M, D);      t[f"h.{l}.mlp.c_proj.bias"] = b(D)
# NO lm_head.weight: tied head (logits = h @ wte.T), like the real HF gpt2 file.

# ---------------- independent numpy reference forward (float64) ----------------
def layer_norm(x, g, bb, eps=1e-5):
    mu = x.mean(-1, keepdims=True)
    var = ((x - mu) ** 2).mean(-1, keepdims=True)      # biased, like cce_tensor_layer_norm
    return (x - mu) / np.sqrt(var + eps) * g + bb

def gelu(x):  # tanh approx, cce_tensor_gelu's exact constants
    return 0.5 * x * (1.0 + np.tanh(0.79788456 * (x + 0.044715 * x ** 3)))

def forward_last_logits(ids):
    x = t["wte.weight"][ids].astype(np.float64) + t["wpe.weight"][:len(ids)].astype(np.float64)
    hd = D // H
    for l in range(L):
        g1, b1 = t[f"h.{l}.ln_1.weight"].astype(np.float64), t[f"h.{l}.ln_1.bias"].astype(np.float64)
        qkv = layer_norm(x, g1, b1) @ t[f"h.{l}.attn.c_attn.weight"].astype(np.float64) \
              + t[f"h.{l}.attn.c_attn.bias"].astype(np.float64)
        Tn = x.shape[0]
        attn_cat = np.empty_like(x)
        for h in range(H):
            q = qkv[:, h*hd:(h+1)*hd]
            k = qkv[:, D + h*hd:D + (h+1)*hd]
            v = qkv[:, 2*D + h*hd:2*D + (h+1)*hd]
            s = (q @ k.T) / np.sqrt(float(hd))
            s = np.where(np.triu(np.ones((Tn, Tn), bool), 1), -1e9, s)
            e = np.exp(s - s.max(-1, keepdims=True)); s = e / e.sum(-1, keepdims=True)
            attn_cat[:, h*hd:(h+1)*hd] = s @ v
        x = x + attn_cat @ t[f"h.{l}.attn.c_proj.weight"].astype(np.float64) \
              + t[f"h.{l}.attn.c_proj.bias"].astype(np.float64)
        g2, b2 = t[f"h.{l}.ln_2.weight"].astype(np.float64), t[f"h.{l}.ln_2.bias"].astype(np.float64)
        mid = gelu(layer_norm(x, g2, b2) @ t[f"h.{l}.mlp.c_fc.weight"].astype(np.float64)
                   + t[f"h.{l}.mlp.c_fc.bias"].astype(np.float64))
        x = x + mid @ t[f"h.{l}.mlp.c_proj.weight"].astype(np.float64) \
              + t[f"h.{l}.mlp.c_proj.bias"].astype(np.float64)
    x = layer_norm(x, t["ln_f.weight"].astype(np.float64), t["ln_f.bias"].astype(np.float64))
    return x[-1] @ t["wte.weight"].astype(np.float64).T          # tied head, no bias

NSEQ, TSEQ = 8, 8
ids = rng.integers(0, V, size=(NSEQ, TSEQ))
gold = np.stack([forward_last_logits(ids[s]) for s in range(NSEQ)]).astype(np.float32)
t["golden.ids"] = ids.astype(np.float32)       # F32 so the C loader can read them
t["golden.logits"] = gold

save_file(t, path, metadata={"n_head": str(H), "n_layer": str(L), "naming": "gpt2"})
print(f"wrote {path}  config L{L} D{D} H{H} V{V} B{B} M{M}  ({len(t)} tensors, "
      f"gpt2 naming, Conv1D [in,out], tied head, {NSEQ} golden seqs)")
