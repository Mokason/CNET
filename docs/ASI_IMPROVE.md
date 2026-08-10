# ASI improve stack (literature slices A–E)

Pure-C module: `include/cnet_asi_improve.h` + `src/cnet_asi_improve.c`  
**Does not lower CERT floors.** Teacher/runtime law unchanged. Episodes never serve.

## Make targets (run in order; each prints PASS + bench)

```bash
cd /home/marble/AI/CNET
make asi_libos            # A — library OS
make asi_defer            # B — conformal + L2D-lite
make asi_route            # C — elbow top-M + HHI + shared/spec
make asi_episode          # D — episodic + continual regression
make asi_firewall_eval    # E — role matrix + anytime-valid compare
make asi_improve_all      # all five
```

## What each slice gives you

| Slice | Marker | API focus |
|---|---|---|
| A Library OS | `ASI_LIBOS_PASS` | capability page, not_for neighbors, precond mask, privilege pick, skill graph edges, 3-stage resolve, janitor debt |
| B Defer | `ASI_DEFER_PASS` | `conf_q` residual gate, `cnet_asi_defer_pick` margin abstain |
| C Route | `ASI_ROUTE_PASS` | `cnet_asi_elbow_topm`, specialization HHI, shared vs specialist kinds |
| D Episode | `ASI_EPISODE_PASS` | ring log, last episode, continual regression count |
| E Firewall/eval | `ASI_FIREWALL_EVAL_PASS` | role×effect allow matrix, Hoeffding anytime compare |

## Resolve law (A+B)

```
query → recall (name | capability substring)
      → executability (world_mask & precond == precond)
      → conformal (if conf_q>=0 and residual>=0 and residual>conf_q → ABSTAIN)
      → least privilege among survivors
```

## Wire into product later

- Call `cnet_asi_resolve` **before** `cnet_mem_resolve` / capsule step with world masks from Unity/robot host.
- Log `cnet_asi_episode_log` after serve outcomes; use `cnet_asi_continual_regressions` in FORM gates.
- Multi-capsule: `cnet_asi_role_allow(role, effect, matrix, …)` before bridge dispatch.
- Director head bake-off: loop trials → `cnet_asi_av_compare` until ±1 (WITHHELD if never separates).

## Product wire (landed)

| Surface | Status |
|---|---|
| `cnet_mem_resolve_ex` + ASI gate | **in** `src/cnet_mem_runtime.c` |
| `cnet_mem_feedback_ex` → episodes | **in** |
| FORM refuses continual regression | **in** |
| `make mem_runtime` product wire checks | **in** tests |
| Unity `AsiProductWire` + composer firewall | **in** AliveValley Director |
| `make asi_av_bakeoff` | **in** `tools/cnet_av_bakeoff.c` |

```bash
make mem_runtime          # includes product wire asserts
make asi_av_bakeoff
cd ../AliveValleyDemo-puppet-master/Tools/HeadlessTestRunner
dotnet run --project MultiCapsuleTestRun.csproj -c Release
```

## Benchmarks (local, 2026-08-08 machine)

| Slice | Bench | Wall (order) |
|---|---|---|
| A | 50k resolve | ~0.002s |
| B | 200k defer + 30k conf resolve | ~0.001s |
| C | 100k elbow | ~0.003s |
| D | 10k episode log | ~0.0002s |
| E | 200k role+av | ~0.001s |

All gates: `make asi_improve_all` → `ASI_IMPROVE_ALL_PASS`.
