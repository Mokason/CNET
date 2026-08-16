# Weight read deliverables 1–3

## 1 Inspect Bonsai tensors — PASS
- 399 tensors, arch=qwen3, 36 layers, hidden=4096
- types: 254×Q1_0 + 145×F32
- tools: `scripts/gguf_weight_peek.py`, `make cnet_gguf_peek`
- artifacts: `result/gguf_weight_peek_bonsai8b/`

## 2 Dequant samples — PASS
- Q1_0 implemented: `w = bit ? +d : -d` (block 18B / 128 w)
- `blk.0.attn_q` sample includes ±0.01416…
- F32 norms readable (output_norm ~2.0–2.1)
- artifact: `dequant_samples.json`

## 3 CNET weight_convert door — PASS (fixture domain)
- `make cnet_weight_convert` → CNET_WEIGHT_CONVERT_PASS checks=44
- real cce_gguf reader, residual_speak=0, certified=0
- domain: synthetic u8_inc16 LUT (not full Bonsai table yet)
- Bonsai Q1 host convert = follow-up (wire Q1_0 into cce_gguf_load_f32)

## Law
offline weights ≠ mouth ≠ CERT
