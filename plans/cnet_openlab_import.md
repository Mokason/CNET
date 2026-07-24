# CNET ← Open-lab import (DeepSeek / Qwen / MoE ideas)

> Gate-or-stale. Non-goals stay non-goals.

**Goal:** Sparse activation across **certified CNET parts** (not a second monolith pretrain).

```text
Open labs:  big net + sparse activation inside
CNET:       sparse activation across many small certified nets
            + rare PEFT on the failing surface
            + refuse when recovery is hopeless
```

---

## P0 — Live MoE-of-CNET

**Route:** goal/skill tag → hard expert (named unit | certified LoRA) → else plan → fault on miss.

| Piece | Path | Gate |
|---|---|---|
| Hard expert try | `cnet_moe_try_hard` before full soft/residual | `make cnet_openlab_import` |
| Fault on miss | existing gap_inbox + `CNET_FAULT_LOG` mirror | bus line or gap note |
| Deploy env | `personal-ai.env` already has FAULT_LOG/STORE | `scripts/cnet_openlab_doctor.sh` |

**PASS:** hard expert serves when unit present; miss notes gap; counters increment.

## P1 — Tiered residual serve (margin-gated)

```text
A certified (plan or hard expert)
  → B medium/soft (margin abstain)
  → C residual only if allow_residual && (no B answer) && residual_bound
  → teacher / gap note
```

| Piece | Env / API | Gate |
|---|---|---|
| Residual skip when warm | already prefer A/B | tier_c only after B fail |
| Optional residual margin floor | `CNET_RESIDUAL_MIN_MARGIN` | abstain residual if soft margin path unused |
| Adapter apply after residual | hybrid_adapter | counter |

## P1 — Accounting dashboard

| Counter | Meaning |
|---|---|
| `tier_a_hits` / `hard_expert_hits` | certified / direct unit |
| `tier_b_hits` | soft/medium |
| `tier_c_hits` / `teacher_forwards` | residual / oracle |
| `gap_notes` / `abstains` | misses |
| `activated_steps` | sum of RoutePlan.length (proxy active compute) |
| `adapter_tick_pass` / `dense_heal` | from tick hooks when available |

**API:** `cnet_acct_snapshot` / `cnet_acct_dump` → `CNET_ACCT_LOG` JSONL  
**Gate:** counters move under hermetic serve sequence.

## P2 — Local-mouth efficiency (policy only)

| Piece | Notes |
|---|---|
| Sparse KV / MLA | remain opt-in on GGUF/DS; doctor prints flags |
| No full MLA retrain | non-goal |
| No chat-weight RL | non-goal |

## Verify

```bash
make cnet_openlab_import   # → CNET_OPENLAB_IMPORT_PASS
scripts/cnet_openlab_doctor.sh
```

## Status

Implemented with `cnet_moe`, `cnet_acct`, personal_ai_serve hard-expert branch, doctor script.
