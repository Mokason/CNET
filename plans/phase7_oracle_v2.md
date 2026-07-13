# Phase 7 — Oracle V2 Evidence-Carrying Lifecycle

**Place:** The boundary where CNET asks deterministic functions, models, tools,
retrieval systems, or human-curated references for bounded answers.

**Dilemma:** A signed callback return can distinguish answer from non-answer,
but cannot honestly distinguish ambiguity, policy refusal, transient failure,
permanent failure, malformed output, or a verifier that cannot decide. Treating
all of those alike contaminates training evidence and makes runtime admission
look more authoritative than it is.

**Consequence:** Oracle v2 makes status, identity, confidence, evidence, and
semantic validity first-class while preserving the v1 callback and requiring an
independent CNET contract before a learned specialist enters the planner.

## Systematic component

The following are deterministic and governed:

- ABI version and structure-size checks.
- Typed input/output representation validation.
- Stable identity over artifact, contract, configuration, retrieval snapshot,
  and toolchain digests.
- Explicit status accounting.
- Semantic validator result: valid, invalid, or undetermined.
- CNB2 identity persistence and digest replay.
- Oracle-to-training-to-certification-to-registry admission.
- Native SoulHost and managed/MCP descriptor projection.

## Irreducible component

The Oracle may still be stochastic, unavailable, policy-restricted, or wrong.
Confidence is testimony, not proof. Evidence artifacts may be incomplete, and a
semantic verifier may return undetermined. CNET preserves these outcomes rather
than coercing them into labels.

## Native contract

`CnetOracleStatus` distinguishes:

1. answer;
2. ambiguous abstention;
3. verifier-undetermined abstention;
4. policy refusal;
5. transient failure;
6. permanent failure;
7. invalid output;
8. invalid input.

`cnet_oracle_invoke` is the canonical serial invocation path. It validates the
ABI, input representation, confidence, output representation, and optional
semantic relation before updating per-status and compatibility counters.

`CnetOracleIdentity` binds behavior to:

- artifact/model;
- contract;
- prompt/configuration/decoding setup;
- retrieval-index snapshot;
- toolchain/environment.

Function and context pointers never participate in identity.

## Lifecycle

```text
bounded descriptor
  → runtime binding
  → typed invocation
  → answer / explicit non-answer
  → evidence accounting
  → exemplar capture
  → student training
  → independent contract certification
  → registry admission
  → runtime evidence and lifecycle controls
```

A CNB descriptor records provenance and binding intent. It is explicitly not a
certificate and not proof that the Oracle is currently bound.

## Relationship to established work

| Established idea | CNET v2 extension |
|---|---|
| OGIS/CEGIS typed teacher query | Heterogeneous runtime ABI plus bounded component identity |
| Snorkel label-or-abstain | Structured typed outputs, semantic validator, certification and admission |
| PATE selective teachers | Non-consensus is one of several explicit non-answer classes |
| Expert Iteration/distillation | Student promotion requires independent contract replay |
| Verifier-guided learning | `valid / invalid / undetermined` is separate from output shape |
| Learning to defer | Policy, transient failure, ambiguity, and verifier uncertainty remain distinct evidence |
| Runtime assurance/PCC | Same native contract controls training evidence and runtime admission |

The defensible contribution is the integrated lifecycle, not the invention of a
callable teacher.

## Implemented tracer bullets

- V2 answer with confidence and evidence digest.
- Ambiguity, policy, transient, permanent, invalid-input, invalid-output, and
  verifier-undetermined outcomes.
- Legacy v1 mapping.
- Governed fallback capture.
- V2 evidence trains a student that independently certifies and registers.
- Oracle runtime adapter uses the same invocation semantics.
- CNB2 round-trip with CNB1 read compatibility.
- Native SoulHost descriptor enumeration.
- Managed `OracleDescriptor` and `SoulHost.Oracles()`.
- MCP `cnet_list_oracles` with explicit
  `descriptor_only_not_runtime_trust` admission state.

## Acceptance

Focused native gates pass:

- `ORACLE_V2_PASS`
- `ORACLE_CONTRACT_ADAPTER_PASS`
- `ORACLE_CNB2_GREEN`
- `ORACLE_SOUL_GREEN`

The closure gate completed successfully:

- `make test` — PASS;
- `make unified` — `CNET_UNIFIED_PASS`;
- `make build` — PASS.

## CPU invocation benchmark

Command: `make oracle_v2_bench`

Method: seven rounds, 20,000,000 calls per round, median,
`CLOCK_MONOTONIC_RAW`, optimized native build. Results on the current host:

| Path | ns/call | calls/s | vs direct v1 |
|---|---:|---:|---:|
| Direct v1 callback | 2.110 | 473,971,825 | 1.00× |
| Governed v1 compatibility | 11.101 | 90,079,601 | 5.26× |
| Governed v2 evidence | 11.711 | 85,386,944 | 5.55× |
| Governed v2 + semantic validator | 12.432 | 80,434,386 | 5.89× |

The v2 metadata path adds 0.610 ns over governed v1. Semantic validation adds
another 0.721 ns. The complete governed and semantically checked path costs
10.323 ns more than a raw callback while sustaining about 80.4 million calls/s.
All measured calls answered successfully and all reject counters remained zero.
