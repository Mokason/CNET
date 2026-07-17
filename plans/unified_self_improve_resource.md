# Unified Self-Improve Resource Program

**Place:** The seams between SoulHost serve, the gap lane, model residency,
deploy knobs, and the bounded improvement engine.

**Dilemma:** The pieces for a full unified self-improving system already exist
(Specialist door, gap lane, health tick, train_fast, int8, TOPK_SET measurement,
bounded GPU offload, improvement-engine bridge), but they are opt-in, partially
offline, and budgeted independently — so the system either sits quiet or burns
multi‑GB teacher residency without compounding capability.

**Consequence:** One deploy profile, one resource governor, budgeted always-on
self-improve (CNB-primary serve, sleeping teacher), set-semantics campaign
recipe, distillation + counterfactual ORDER_ONLY ranking among certified peers,
and recipe proposals that never lower certification bars.

## Hypotheses

- **H0:** documentation only, defaults unchanged, no gate markers, or quality
  bars relaxed to raise yield/speed.
- **H1:** hermetic gates prove (1) deploy profile applies measured free wins,
  (2) resource governor enforces RAM/VRAM/teach-rate budgets fail-closed,
  (3) gap lane respects per-tick closure caps and TOPK_SET teacher semantics,
  (4) self-improve distillation mints certified chunks only through consolidate,
  (5) counterfactual order is ORDER_ONLY among certified peers,
  (6) recipe proposals are evidence-backed and never mutate thresholds.

## Phases

| Phase | Deliverable | Gate |
|---|---|---|
| 1 | Deploy profile + resource governor + teacher-idle budget fields | `make resource_governor`, `make deploy_profile` |
| 2 | TOPK_SET in gap-lane teacher, max closures/tick, distill path | `make self_improve`, gap_lane still green |
| 3 | CF ORDER_ONLY ranker + harness-oracle admit path | `make counterfactual_order`, specialist path |
| 4 | Recipe proposal tool + deploy service knobs | `make recipe_proposals`, service config |

## Quality floor (non-negotiable)

- Certification / Wilson / PROOF bars are never lowered by this program.
- Counterfactual never outranks certification.
- Teacher sleep must not invent provenance; unload is only when idle.
- v2 TOPK_SET campaigns require new goldens (set semantics change unit claims).

## Result

**H1 achieved (hermetic, 2026-07-17):**

```
make unified_self_improve  →  UNIFIED_SELF_IMPROVE_PASS
  resource_governor        →  RESOURCE_GOVERNOR_PASS (23)
  counterfactual_order     →  CF_ORDER_PASS (8)
  self_improve             →  SELF_IMPROVE_PASS (17)
  deploy_profile           →  DEPLOY_PROFILE_PASS
  recipe_proposals         →  RECIPE_PROPOSALS_PASS
  gap_lane_service_config  →  GAP_LANE_SERVICE_CONFIG_PASS
```

Regression: `make gap_lane` → `GAP_LANE_PASS` (51); `make acquire` → 131 checks.

Operator next steps (not required for the hermetic gate):
1. `source scripts/apply_deploy_profile.sh` before MCP / gap-lane
2. Rebuild `make gap_lane_run_build` and restart the user unit when ready
3. Run Qwythos v2 with `config/qwythos_v2_campaign.env` + **new** goldens only
