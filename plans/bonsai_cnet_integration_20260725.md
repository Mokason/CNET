# Bonsai (PrismML) x CNET integration (2026-07-25)

## Summary

Integrate PrismML **Bonsai-8B (1-bit Q1_0)** as CNET **Tier-C residual + light teacher plane** via HTTP, without claiming CNET "learns at 1-bit". CNET still certifies PEFT/BTN units; Bonsai is the cheap coherent residual brain.

## P0 — Serve

| Item | Result |
|---|---|
| Broken `llama.cpp-prism` 203/EXEC | Replaced with `/home/marble/AI/llama.cpp-fallback/build-rocm/bin/llama-server` |
| `bonsai-server.service` | Enabled, ConditionFileIsExecutable, CPU default (`-ngl 0`) |
| Smoke 5 prompts | 5/5 coherent (ping, 4, Paris, haiku, JSON) |
| CPU median tok/s | ~9.1 (lane residual OK) |
| GPU_0 ngl=99 smoke | ~47 tok/s; VRAM ~6% on GPU0 (optional drop-in) |
| GPU_1 alone | HIP build sees no device when `ROCR=1` only |

## P1 — HTTP residual + light lane

| Item | Result |
|---|---|
| `src/residual_http.c` | Window one-hot residual via `/completion` + top_logprobs |
| Gate | `make residual_http` + `residual_http_real` → **RESIDUAL_HTTP_PASS checks=9** |
| SoulHost / personal_ai | Prefer `CNET_RESIDUAL_HTTP` over hermetic/GGUF |
| Env | `personal-ai.env` + MCP `launch.sh` + hotswap template |
| Gap lane | Drop-in `no-heavy-teacher.conf` — **no Gemma GGUF in-process** |
| Lane RAM | **~24G → ~0.45G** |

## P2 — PEFT bus traffic + eval

| Item | Result |
|---|---|
| `bin/bonsai_residual_fault_seed` | 16/16 residual pairs → fault bus |
| `logs/cnet_faults.jsonl` | **16 lines** (was 0) |
| Eval | `logs/bonsai_cnet_eval.json` + smoke JSON |

Note: adapters not auto-certified from residual window unit yet; bus is no longer empty. Nightly `registry_lora` tick / fault_loop can consume.

## P3 — Ternary-27B + vision

| Item | Status |
|---|---|
| Download `Ternary-Bonsai-27B-Q2_0.gguf` | Started under `Models/Ternary-Bonsai-27B-gguf/` (resumable curl) |
| Runtime | Needs Prism ternary kernels or mainline `Q2_0_g64` + matching file; swap residual URL when served |
| Vision mmproj | Deferred — not blocking PEFT library |

## How to run

```bash
systemctl --user status bonsai-server cnet-personal-ai-lane
curl -s http://127.0.0.1:8080/v1/models | head
export CNET_RESIDUAL_HTTP=http://127.0.0.1:8080
export CNET_RESIDUAL_WINDOW=$PWD/english_window_256_bonsai.txt
make residual_http_real
# seed PEFT pairs
CNET_FAULT_LOG=$PWD/logs/cnet_faults.jsonl ./bin/bonsai_residual_fault_seed 32
```

MCP: recycle gateway externally so `cnet.so` + launch env reload.

## Honesty

- Bonsai does **not** replace certified JTC/procedure learning.
- Native `cce_gguf` still cannot open Q1 Bonsai (HTTP path is required).
- Brochure RTX 5090 numbers are not ROCm R9700 claims; measured CPU/GPU smokes are.
