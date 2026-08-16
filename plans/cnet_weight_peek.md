# Weight read 1–3 — Bonsai-8B offline

Status: **DONE — OFFLINE ONLY — NOT MOUTH — NOT CERT**

## 1) Inspect tensors

```bash
# Python (gguf)
./scripts/gguf_weight_peek.py /home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf \
  --out result/gguf_weight_peek_bonsai8b

# C (cce_gguf)
make cnet_gguf_peek
```

Bonsai-8B:

| | |
|--|--|
| arch | qwen3 |
| layers | 36 |
| hidden | 4096 |
| heads / kv | 32 / 8 |
| tensors | **399** |
| types | **254 × Q1_0 (41)** + **145 × F32** |
| size | ~1.16 GB |

## 2) Dequant samples

Q1_0 law (llama.cpp): block = `fp16 d` + 16 bytes bits (128 w).

`w[j] = bit ? +d : -d`

Example `blk.0.attn_q.weight` sample8:

`[-0.01416, -0.01416, +0.01416, +0.01416, …]` absmean≈0.027

F32 norms e.g. `output_norm.weight` ≈ `[2.125, 1.92, 2.05, …]`

Artifacts:

- `result/gguf_weight_peek_bonsai8b/tensors.json`
- `result/gguf_weight_peek_bonsai8b/dequant_samples.json`
- `result/gguf_weight_peek_bonsai8b/PEEK.md`

## 3) CNET weight_convert door

```bash
make cnet_weight_convert
# CNET_WEIGHT_CONVERT_PASS checks=44
# host_gguf=0 fixture_through_cce_gguf=1 certified=0 residual_speak=0
```

This door:

- Uses **real** `cce_gguf_load` / `load_f32` / `tensor_bytes`
- Domain today: **synthetic `u8_inc16` LUT fixture** (not full Bonsai→table yet)
- **No auto-CERT**, residual mouth counters stay 0
- Bonsai Q1_0 host conversion = future slice (needs Q1_0 in `cce_gguf_load_f32` + domain plan)

## Law

- Offline weight I/O ≠ OPEN_CHAT draft  
- Weight read ≠ CERT claim  
- Live `:8081` / MAX still do not export W over HTTP  
