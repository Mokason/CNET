# MAX / Mojo → CNET held_model_v1

Status: **LIVE residual teacher — float32 on R9700 — never auto-CERT — WITHHELD as CNET mouth**

## Contract

CNET leftover hops use `held_model_v1` (`cnet_held_model_ask`):

1. test hook  
2. `CNET_HELD_MODEL_PATH` → GGUF harness plugin  
3. `CNET_HELD_MODEL_ENDPOINT` → OpenAI chat-completions HTTP  

Modular **MAX** (`max serve`) exposes the same OpenAI shape. **Mojo** ships inside the `max-all` package used to run MAX kernels.

## Wiring (this machine)

| Piece | Value |
|---|---|
| MAX install | `~/modular-max-nightly` pixi `max-all` **26.6.0.dev** (stable `~/modular-max` also present) |
| Lifecycle | `scripts/run_cnet_max_held.sh {start\|stop\|status\|smoke}` |
| Env | `config/cnet-max-held.env` |
| Endpoint | `http://127.0.0.1:8000/v1/chat/completions` |
| Devices | `gpu:0` (R9700 / gfx1201). Bonsai compete baseline stays on `:8081` / GPU1 |
| Model | `Qwen/Qwen2.5-7B-Instruct` |
| Encoding | **`float32`** (required) |
| Context | `max-length=4096`, `max-batch-size=4`, `--no-device-graph-capture` |

```bash
scripts/run_cnet_max_held.sh start
scripts/run_cnet_max_held.sh smoke   # expects pong
set -a && . config/cnet-max-held.env && set +a
```

## AMD gfx1201 root cause

| Path | Result |
|---|---|
| HF transformers bf16 ROCm | coherent (`4`) |
| MAX GPU **bfloat16** (stable + nightly) | deterministic garbage (`strugg'icon…`) |
| MAX GPU **float32** | coherent (`4`, `pong`) |
| MAX CPU float32 | coherent |

Do **not** serve bf16 on this box until Modular fixes AMD bf16 kernels for gfx1201.

## Laws

- Residual / teacher only — drafts may land in miss_log / grow distill; **never self-CERT**.
- Do **not** swap compete baseline macros in `cnet_compete_eval.h` to MAX unless a new measured compete run is intentional.
- `CNET_NEVER_VOICE_LLM=1` remains default for ROE mouth.

## Benchmark

```bash
cd ~/modular-max-nightly
pixi run max benchmark \
  --backend modular --base-url http://127.0.0.1:8000 \
  --model Qwen/Qwen2.5-7B-Instruct --tokenizer Qwen/Qwen2.5-7B-Instruct \
  --endpoint /v1/chat/completions \
  --dataset-name random --random-input-len 128 --random-output-len 64 \
  --max-output-len 64 --num-prompts 48 --max-concurrency 1,2,4 \
  --no-collect-gpu-stats \
  --result-filename results/cnet-max-qwen7b-f32-r9700.json
```

Note: `--collect-gpu-stats` crashes on this ROCm (`RSMI_STATUS_NOT_SUPPORTED` memory busy %). Always pass `--no-collect-gpu-stats`.

## Gate markers

```text
scripts/run_cnet_max_held.sh status   # ready:true
scripts/run_cnet_max_held.sh smoke    # CNET_MAX_HELD_SMOKE_PASS content_ok
bin/test_held_max                     # CNET_HELD_MAX_OK out=[pong]
```

## Benchmark result (full sweep 1/2/4, 2026-08-16)

| mc | ok/fail | req/s | out tok/s mean | out tok/s med | TTFT mean ms | TTFT med ms | TPOT med ms | ITL mean ms | duration s |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 48/0 | 0.1374 | 20.70 | 20.97 | 4277 | 4366 | 47.69 | 48.14 | 349 |
| 2 | 44/0 | 0.1203 | 7.53 | 7.65 | 8401 | 8295 | 130.64 | 132.15 | 366 |
| 4 | 40/0 | 0.1722 | 7.26 | 7.33 | 14635 | 14573 | 136.45 | 137.26 | 232 |


Artifacts under `result/max_qwen7b_f32_r9700_*`.


## Hemisphere split

MAX/Bonsai/held = **RESIDUAL** only. See `plans/cnet_hemisphere.md` and `make cnet_hemi`.
CORE (skills/capsules/math) always first; residual never auto-CERTs.

## Default vs optional

Default held residual is **Bonsai** (`config/cnet-bonsai-held.env`). This file is **optional MAX** override only.
