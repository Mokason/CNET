#!/usr/bin/env python3
"""Generate a DIFFERENT-config transformer in Supra's safetensors naming, so the
cce_transformer_qat load+parity path can be exercised on a model that is NOT
Supra. The Supra decomposer (cce_supra_load_decomposed) hardwires 4 layers /
4 heads and Supra's tensor names, and cce_supra_decomposed has fixed [4] LN
arrays -- so this stays 4-layer/4-head but changes every FREE dimension
(n_embd, vocab, block, mlp). Weights are random (small init); the point is the
loader/trainer/forward handling a different config, proven by forward parity,
not any learned behavior.

Writes ./altmodel_cache/model.safetensors. Weights follow the PyTorch Linear
[out, in] convention the real Supra checkpoint uses (the loader transposes to
the forest's [in, out]).

Usage: python3 tools/gen_altmodel.py [out_dir]
"""
import sys, os
import numpy as np
from safetensors.numpy import save_file

L, D, H, V, B, M = 4, 128, 4, 2000, 96, 512   # n_layer(=4), n_embd, n_head(=4), vocab, block, mlp
out_dir = sys.argv[1] if len(sys.argv) > 1 else "altmodel_cache"
os.makedirs(out_dir, exist_ok=True)

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

path = os.path.join(out_dir, "model.safetensors")
save_file(t, path)
print(f"wrote {path}  config L{L} D{D} H{H} V{V} B{B} M{M}  ({len(t)} tensors)")
