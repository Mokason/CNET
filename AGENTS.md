# AGENTS.md — read before changing CNET

## What CNET is building

> **CNET builds ASI — Artificial Specialized Intelligence. In CNET, ASI never
> means Artificial Superintelligence. CNET is not claiming AGI. Broad semantic
> grounding exists to understand intent; deep competence comes from isolated,
> certified, portable Micro Tensor Kernels. Wikipedia-like accumulation may
> broaden coverage and composition over time, but unit count alone is not
> intelligence and all broader claims remain benchmark-gated.**

```
base semantic grounding → typed intent → CNET isolated knowledge registry
      → certified kernels → verifier / abstention → residual teacher (uncovered)
```

If you are about to write "AGI", "general intelligence", "superintelligence",
or "reasoning" about CNET in a doc, commit message, or log marker: don't. Say
which gate measured what, and mark the rest WITHHELD.

## Two things that are NOT the same

| | MTK cartridge (`.tskill` / CMSK / MTSK) | Certified unit / capsule |
|---|---|---|
| Source | `include/cce/cce_mtk.h` | `include/base.h`, `include/cnet_capsule.h` |
| What it is | weight deltas patched onto a host model's named tensor sites | CNU1-sealed BTN + Contract exemplars + ports |
| Typed contract | no | yes |
| Certification | no | yes (behaviour digest, contract verify) |
| Coverage / abstention | no | yes (travels inside the capsule) |
| Portable across bases | only to a matching host model | yes, with explicit compatibility refusal |

Do not equate them, and do not build a second packaging system. Portable
knowledge extends `cnb_export_subset` + the capsule manifest.

## Non-negotiables

1. **Never lower a certification floor to make a gate pass.** Report the failure.
2. **Fail closed, or fail loud.** A silently-degraded guard is worse than a red gate.
3. **No vanity metrics.** A `1.000` must name what was tested and what was not.
4. **Anti-collapse:** never train on CNET's own Tier-A answers. External teacher,
   user correction, or verified tool result only.
5. **AMD/ROCm only.** No CUDA-only paths.
6. **TDD for production behaviour:** failing test with its RED marker first.

## Gates worth knowing

| Gate | Proves |
|---|---|
| `make knowledge_accumulation_bench` | isolation, interference, portable round-trip, corruption/incompatibility refusal, OOD abstention |
| `make knowledge_capsule` | one certified capability transfers with its gate intact |
| `make coverage_abstain` | a unit never answers outside its certified domain |
| `make own_learning_health` | refuses dangerous unattended mine/serve configurations |
| `make capability_cert` | held-out capability floors (two graded, not all binary) |

Decision records live in `plans/*.md` — this repo has no separate ADR tree.
