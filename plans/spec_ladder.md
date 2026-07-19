# Progressive specialist conversion ladder

**Status:** landed — hermetic gate + comparison bench green

## Why

Whole-forest post-hoc ternary collapses quality. Bonsai-style whole-model 1-bit
needs QAT of everything. CNET path: **promote specialists under a reconstruction
contract**, family-by-family, fail-closed.

## Ladder

1. **Capture** real activations via `cce_gguf_set_capture_hook` (calib forwards)
2. **Schedule** stages: gate/up (looser bar) → down (stricter) → attn
3. Per specialist: **STE QAT** (or OBQ/posthoc) → ternary → **certify** holdout relerr
4. **PASS** → `pack_trits` + freeze; **FAIL** → restore FP
5. **Export** certified-only pack (magic `LDTR` v1) — never force-posthoc the rest

## API / tools

| | |
|--|--|
| Core | `include/cce/cce_spec_ladder.h`, `src/cce/cce_spec_ladder.c` |
| Gate | `make spec_ladder` → `SPEC_LADDER_PASS` (37 checks) |
| CLI | `make spec_ladder_tool` → `bin/cnet_spec_ladder MODEL [schedule\|family] [ste]` |
| Bench | `make ladder_bench` → `LADDER_BENCH_PASS` |

```bash
make spec_ladder ladder_bench

# Real model (slow open; use FP shadows)
CNET_INFER_FP=1 CNET_LADDER_MAX=16 CNET_LADDER_EXPORT=out.ldtr \
  bin/cnet_spec_ladder hermes_wrappers/Qwythos-9B-*.gguf schedule ste
```

## Comparison (tiny hermetic GGUF, measured)

| Path | logit_relerr vs FP | argmax agree | decode tok/s | packed |
|------|-------------------:|:------------:|-------------:|-------:|
| FP baseline | 0.0000 | yes | ~3.1e5 | 0 |
| **Posthoc all** | **0.0114** | yes | ~2.9e5 | 14/14 |
| **STE + schedule + real acts** | **0.0051** | yes | ~2.6e5 | **5 certified** |

**Verdict:** STE schedule **~2.2× lower** logit error than whole-forest posthoc,
while only packing specialists that pass the bar (fail-closed). Tiny-model tok/s
is noise-dominated (micro model); quality is the signal.

Certified example (real capture, bar 0.40 for gate/up):

- `gate_proj` / `up_proj` layers 0–1: **cert=1**, relerr ~0.38–0.40
- `down_proj` / attn at bar 0.28–0.35: often **cert=0** (left FP) — correct fail-closed

## Schedule defaults

| Stage family | cert_relerr | STE steps |
|--------------|------------:|----------:|
| gate_proj / ffn_gate / up_proj / ffn_up | 0.40 | 48 |
| down_proj / ffn_down | 0.28 | 64 |
| o_proj / attn_output | 0.32 | 48 |
| q/k/v_proj / attn_qkv | 0.35 | 40 |

## Qwythos multi-day campaign

Real Qwythos-9B Q4 (`qwen35`, 249 branches, D=4096):

| Phase | Wall (measured) |
|-------|-----------------|
| Load FP forest | ~4 min |
| Capture (2–4 seqs) | ~0.5–1 min |
| 8× gate STE (12 steps) | ~2.6 min |
| Empirical gate posthoc relerr | **~0.51** |

### Campaign (single process — 3 speed rules)

1. **One process, full `schedule`** — load once, all families (no per-family reload)
2. **Import + skip** — `CNET_LADDER_IMPORT` + skip already-packed specialists
3. **Lean STE** — default steps=32, n_calib=48 (do not inflate unless quality needs it)

Also: dual **hip + OpenCL** for STE; **checkpoint export after each stage**.

```bash
# Start (detached)
nohup bash scripts/qwythos_ladder_campaign.sh \
  > logs/qwythos_ladder_campaign.out 2>&1 &

# Monitor
bash scripts/qwythos_ladder_status.sh
tail -f logs/qwythos_ladder_campaign.log
```

Resume after kill/reboot: same script; imports
`artifacts/qwythos_ladder_campaign.ldtr` and skips packed names.

Default cert **0.56**, steps **32**, calib **48**.

## Qwythos quality reality (2026-07-19)

| Observation | Implication |
|-------------|-------------|
| Posthoc ternary holdout relerr on FFN | **~0.51–0.53** (Q4 dequant → ternary floor) |
| STE train MSE can collapse | **Overfits calib** if selected on train loss |
| Fixed STE | Holdout checkpoint + L2→W0 + **min(posthoc, STE)** |
| Pack-all FFN at cert 0.56 | **Breaks English** (2+2 blank) |
| Pack **1** gate at relerr 0.515 | **Coherent** (2+2 → 4) |
| Deploy path | **Greedy e2e pack**: one specialist at a time + ChatML quality gate |

**LDTR v2:** exports **every** packed block in a cascade (`branch_name` + `block_idx`).  
Import still reads **v1** (block 0 only).

**Greedy policy:** `FAM_REJECT_LIMIT=0` (default) = maximal; `=2` = faster early-exit per family.

Scripts:
- `scripts/verify_e2e_pack_logic.sh` — offline helpers
- `scripts/qwythos_e2e_greedy_pack.sh` — live quality-gated pack
- `CNET_LADDER_IMPORT=` on `cnet_quality_eval` — ChatML + GPU

## Next levers

- Better than ternary for hard layers (int4/AWQ-style) when e2e rejects
- Larger real-act calib / multi-prompt quality suite
- KV-cache compression (orthogonal; e.g. TurboQuant-class)
