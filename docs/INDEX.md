# CNET Documentation Index

**Current HEAD focus (2026-07-21):** compositional primitives + **use-loop**
(serve → evidence → distill → teach) + **Oracle teacher runtime** governance.

Start here. Every claim below must name a gate; if the gate is missing, the
claim is stale.

## One-page orientation

| Question | Answer | Gate / path |
|---|---|---|
| What is CNET? | Finite, typed, recoverable intermediates so neural parts compose like software | README *The Loop* |
| What is the core type? | One `Specialist` admitted only through `specialist_admit` | `make specialist_unit` |
| Who decides which unit runs? | Three layers: recall / synthesis / policy | [`dispatch.md`](dispatch.md) · `make dispatch_story` |
| How does learning close? | Miss → gap → oracle teach → certify → seal → serve | `make gap_lane` · `make acquire` |
| How does evidence stick? | Per-base `<base>.state/*.stats` + `soul_serve.stats` on close/open | `make serve_feedback` · `make cnet_deep_use_loop` |
| How are teachers governed? | Oracle v2 + Tier A/B teacher runtime (attest/lease/scorecard/batch) | `make oracle_v2_test` · `make oracle_teacher_runtime` |
| What is the product loop? | Personal AI: local certified first; residual/teacher on miss; tick seals | `make personal_ai` · `make post_seal_serve` |
| End-to-end use-loop umbrella | Deep multi-priority + product surfaces | `make cnet_use_loop_acceptance` |

## Document altitude

| Doc | Job |
|---|---|
| [`../README.md`](../README.md) | Thesis, verified-today table, quick start |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | Layer-by-layer mechanism (Specialist, CCE, planner, autonomy, edge) |
| [`dispatch.md`](dispatch.md) | One dispatch story (recall / synthesis / policy) |
| [`CHANGELOG.md`](CHANGELOG.md) | Dated optimization ledger + negative results |
| [`EXECUTION_TIERS.md`](EXECUTION_TIERS.md) | Core vs acceleration vs quarantined paths |
| [`RELEASE_POLICY.md`](RELEASE_POLICY.md) | Private release authority; no public push by default |
| [`verified-today.generated.md`](verified-today.generated.md) | Machine claim ledger (`make claims`) — regenerate, do not hand-edit |
| [`hermes_hosting.md`](hermes_hosting.md) | Hermes/MCP hosting notes |
| [`phase123_benchmark_closure.md`](phase123_benchmark_closure.md) | Benchmark taxonomy (measured / contract / withheld) |
| [`cnet-history.md`](cnet-history.md) | Long chronology |

## July 2026 use-loop plans (current)

| Plan | Gate |
|---|---|
| [`../plans/deep_eight_priorities.md`](../plans/deep_eight_priorities.md) | `make cnet_deep_use_loop` |
| [`../plans/six_priority_improvement.md`](../plans/six_priority_improvement.md) | `make cnet_use_loop_acceptance` |
| [`../plans/oracle_teacher_runtime.md`](../plans/oracle_teacher_runtime.md) | `make oracle_teacher_runtime` |
| [`../plans/personal_ai_local_first.md`](../plans/personal_ai_local_first.md) | `make personal_ai` |
| [`../plans/hybrid_universal_architecture.md`](../plans/hybrid_universal_architecture.md) | `make hybrid_ai` |
| [`../plans/unified_self_improve_resource.md`](../plans/unified_self_improve_resource.md) | `make unified_self_improve` |
| [`../plans/delegation_master_report.md`](../plans/delegation_master_report.md) | Master execution report |

## Primary make umbrellas

```sh
make test                      # native verification chain
make unified                   # CPU-only vertical (native + cnet.so + .NET + MCP)
make cnet_use_loop_acceptance  # use-loop product umbrella (includes deep loop)
make cnet_deep_use_loop        # P1–P8 deep multi-priority hermetic
make oracle_teacher_runtime    # Oracle A+B teacher governance
make oracle_v2_test acquire    # oracle + acquisition regression
make priority_acceptance       # release-priority bundle (when tree clean)
make release_integrity         # single clean-tree release authority
make claims                    # regenerate verified-today.generated.md
```

## Honesty rules (documentation law)

1. **Gate or date.** A current claim names a make target or is explicitly historical.
2. **Withheld ≠ pass.** FACTOR/TruthfulQA/LongBench stay withheld until a measured path runs.
3. **Descriptor ≠ trust.** Oracle CNB descriptors are provenance until bind + certify + admit.
4. **Synthetic ≠ quality.** Modeled speedups and tokenizer-only gates are labeled as such.
5. **Regenerate claims.** Never hand-edit `verified-today.generated.md`.

## Status snapshot (2026-07-21)

- Use-loop + evidence persist: green under `CNET_USE_LOOP_ACCEPTANCE_PASS` / `CNET_DEEP_USE_LOOP_PASS`
- Oracle teacher runtime A+B: green under `ORACLE_TEACHER_RUNTIME_PASS`
- Live MCP library may still show flat reliability until real serve traffic hits SoulHost — hermetic gates prove the mechanism; production evidence requires live serves
