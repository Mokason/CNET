# Hybrid Universal Architecture (A/B/C)

**Place:** Personal AI serve path — close the “open-ended dense wins” gap without
abandoning certified skills.

**Tiers**

| Tier | Name | Authority |
|---|---|---|
| A | Certified skills | PROOF/SAMPLED units; highest trust |
| B | Soft specialists | Evidence/margin-gated; provisional; never silent PROOF |
| C | Residual generator | Local dense-ish residual or remote teacher; labeled uncertified |

**Policy:** A → B → C; promote structure from C successes into A when mineable.

**Roadmap P0–P6:** residual serve, soft specialists, distill, medium modules,
personal adapter, structure mining, benchmarks.

**Gate:** `make hybrid_ai` → `HYBRID_AI_PASS`; `make hybrid_bench` → benchmarks.

## Result (hermetic, 2026-07-17)

```
make hybrid_ai    → HYBRID_AI_PASS checks=21
make hybrid_bench → HYBRID_BENCH_PASS
  trials=200
  hybrid_skill_acc=100%
  hybrid_open_acc=100%
  dense_open_acc=100%   (rot1 dense prior)
  hybrid_ops_ratio=0.625  (cheaper active work than always-on dense)
  local_or_soft_rate=75% residual_rate=25%
```

JSON: `logs/hybrid_bench.json`

## Real Tier C residual (GGUF, 2026-07-17)

Port-shaped residual over a fixed next-token window — not full-vocab free text.
When `CNET_RESIDUAL_GGUF` is set, `personal_ai_auto_residual_gguf` opens the
local transformer and serves open-ended requests as uncertified Tier C.

```
# Hermetic (no model required)
make residual_gguf → RESIDUAL_GGUF_PASS

# Real smoke (operator machine)
CNET_ORACLE_INT8=1 \
CNET_RESIDUAL_GGUF=/home/marble/AI/Models/gemma4-v2-Q4_K_M.gguf \
CNET_RESIDUAL_WINDOW=/home/marble/AI/CNET/english_window_256.txt \
make residual_gguf_real
→ RESIDUAL_GGUF_PASS checks=9
  real residual has window / vocab
  oracle forward succeeds (one-hot in window)
  personal_ai serves real residual as Tier C
```

Log: `logs/residual_gguf_real.log`. Deploy knobs enabled in
`config/personal-ai.env` (`CNET_RESIDUAL_GGUF` + window). Learner systemd
unit loads the env file; `gap_lane_run` ignores residual (teacher-only);
`personal_ai_open` auto-binds for serve/mine tools.

### P5 real structure mine (same run)

```
make residual_structure_mine_real
→ residual traces accumulate (min_hits=3)
→ mine first-16 window slots labeled by residual GGUF
→ admit unit into registry
→ post-mine serve source=local Tier A
RESIDUAL_GGUF_PASS checks=15
```

