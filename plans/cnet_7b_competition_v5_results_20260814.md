# CNET-ASI-5 v5 frozen benchmark result

Status: **TERMINAL PASS — BOUNDED SUITE ONLY**

On 2026-08-14, commit `3165f882ce59f40c7d97d091e7933eb539c0a6f6`
(tree `7b45d8bba30dcfbfc0104501852cc03cd3f44d71`) completed the
one-command native benchmark:

```text
make -j1 cnet_7b_compete_results
```

The run issued all 448 frozen rows once to each backend and emitted exactly one
terminal marker:

```text
CNET_7B_COMPETE_PASS suite=CNET-ASI-5-v5 cnet_exact=432/448
baseline_exact=214/448 claim=bounded_suite_only broader_claims=WITHHELD
```

This is an authenticated comparison on the frozen CNET-ASI-5-v5 suite. It is
not a claim about open-ended chat, coding, or general intelligence. Broader
claims remain **WITHHELD**.

## What was compared

| Side | Identity |
|---|---|
| CNET | Native C runtime, 321,757-parameter WordLM + six certified capsules |
| Baseline | Pinned Bonsai 8B GGUF on AMD/ROCm (`bonsai_8b_rocm_q1_0`) |
| Baseline endpoint | `http://127.0.0.1:8081/v1/chat/completions` (`-ngl 99`) |
| Baseline SHA-256 | `284a335aa3fb2ced3b1b01fcb40b08aa783e3b70832767f0dd2e3fdfa134bd54` |
| Suite | 448 rows: 64 per certified contract, 128 OOD |
| Candidate freeze | `S11` `34e6d4670d58b5a84ced6d97b046d5b3e060c93c` |
| Fixture freeze | `F11` `3165f882ce59f40c7d97d091e7933eb539c0a6f6` (records `S11`; fixture bytes stay `F5`) |

The baseline is the labeled AMD/ROCm pin, not the earlier CPU `-ngl 0` pin.
No CUDA path was used. No Python ran in the compete lane.

## Aggregate result

| Metric | CNET native C | Bonsai 8B ROCm | Outcome |
|---|---:|---:|---|
| Overall exact decisions | 432/448 (0.964285714) | 214/448 (0.477678571) | CNET +48.66 percentage points |
| Covered exact decisions | 304/320 (0.950000000) | 87/320 (0.271875000) | CNET beat baseline and met the 0.95 floor |
| Covered answer coverage | 304/320 (0.950000000) | 159/320 answered, 87 correct | CNET met the fixed 0.95 floor |
| Selective accuracy | 304/304 (1.000000000) | 87/160 (0.543750000) | CNET passed the fixed 0.99 floor |
| OOD correct abstention | 128/128 (1.000000000) | 127/128 (0.992187500) | CNET passed |
| Unsafe OOD answers | 0 | 1 | CNET passed zero-unsafe floor |
| Contract violations | 0 | 8 | CNET passed zero-violation floor |
| Invocation failures | 0 | 0 | Both clean |
| Residual calls | 0 | 0 | CNET passed |
| Composition hop guards | 192 | — | 64 answered compositions × 3 hops |
| Median latency | 0.473770 ms | 212.532555 ms | CNET 448.6x faster |
| p95 latency | 0.680250 ms | 479.364225 ms | CNET 704.7x faster |

Every CNET covered answer was correct. Its 16 wrong-looking overall misses
are covered CRC abstentions (WordLM did not propose CRC), not wrong
substantive answers. No individual v5 backend response, prompt/output pair,
or per-row diagnostic is published here.

## Per-lane aggregate result

| Lane | CNET | Bonsai 8B ROCm |
|---|---:|---:|
| `increment_mod256` | 64/64 | 21/64 |
| `minutes_to_seconds` | 64/64 | 63/64 |
| `crc8_atm` | 48/64 | 1/64 |
| `access_policy_v1` | 64/64 | 0/64 |
| `compose3_mod256` | 64/64 | 2/64 |
| unsupported / OOD | 128/128 | 127/128 |

## Size evidence

| Property | CNET | Bonsai 8B | Ratio |
|---|---:|---:|---:|
| Learned base parameters | 321,757 | 8,188,548,096 | 0.000039294 (25,449x smaller) |
| Complete artifact bytes | 534,601 | 1,158,654,496 | 0.000461398 (2,168x smaller) |
| Base artifact bytes | 277,687 | — | — |
| Capsule artifact bytes | 255,109 | — | — |
| Sealed capsule payload bytes | 192,352 | — | — |

Artifact manifest SHA-256:
`81fe446218431d7520a7a2d4309e069600ae11be0d3d73e92e04dea78cb7c009`.

## What the candidate kept

Two-gate admission stayed in place: calibrated WordLM intent proposal, then an
independent typed semantic frame. Certified capsules, coverage checks at every
compose hop, OOD abstention, corruption refusal, and incompatibility refusal
were not widened to make a gate pass. CNET did not train on its own Tier-A
answers.

The remaining 16 CRC abstentions are intent-proposal refusals. The WordLM
was not retrained after the v5 candidate freeze; S11 reuses the pinned
artifact when hashes already match.

## Recovery path that produced this PASS

Earlier official v5 runs failed without lowering floors:

| Freeze | Terminal | Notes |
|---|---|---|
| `F5` | FAIL `workflow_stage` | Extra development TSV in the sealed artifact tree |
| `S6`/`F6` | FAIL four score gates | Covered 80/320; increment 0, policy 0 |
| `S7`/`F7` | FAIL four score gates | Same lane aggregates |
| `S9`/`F9` | FAIL two score gates | Covered 96/320; beat baseline overall |
| `S10`/`F10` | FAIL `workflow_stage` | Development semantic gate `unsafe=1` |
| `S11`/`F11` | **PASS** | Covered 304/320, OOD 128/128, guards 192 |

S11 closed the development OOD "permission one" output assertion and reused
the pinned WordLM instead of reproducing it on CPU. The F5 fixture bytes
never changed.

## Preserved evidence

| Evidence | SHA-256 |
|---|---|
| `logs/cnet_7b_compete_results.log` | `258e44491cb8eec31136e771c21137989eecb446f887872069ee204ec86fff46` |
| `logs/cnet_7b_eval_build.log` | `654ccdbfc55910e973472d7eb47e21f3b07446d30584af046d9c4c0ef2ef1ed1` |
| `logs/cnet_7b_baseline_run.log` | `83c7dd8f004aaedcd9d570808f51d60a17ad88c857cf52bf427c216b2ff1baaf` |
| `logs/cnet_7b_cnet_run.log` | `e7b2c0afc70ec7dd02653373c35f1a4f78f8942753aefbfd66ffe409e8d1928e` |
| `logs/cnet_7b_baseline_preflight.log` | `52e37b505c31402178a2c2ad2dbc0acbe9042175a84647c467e659db02ae6be9` |

Canonical journals and anchors are owner-private under
`/home/marble/.local/state/cnet/cnet_asi5_v5/results/`. Prior FAIL journals
remain beside that root as `cnet_asi5_v5_s6f6_fail`,
`cnet_asi5_v5_s7f7_fail`, and `cnet_asi5_v5_s9f9_fail`.

## How to read this result

The authenticated PASS is commit `F11`. A later documentation commit on the
same branch is not part of that freeze. Re-running
`make -j1 cnet_7b_compete_results` from a commit after `F11` is a new freeze
cycle, not a second result path on `F11`.

Allowed claim: on the frozen CNET-ASI-5-v5 suite, native CNET beat the
pinned Bonsai 8B ROCm baseline on overall exact decisions, covered exact
decisions, OOD abstention, selective accuracy, safety, contract
violations, and latency, and it met every preregistered score floor.

Disallowed claim: CNET is generally stronger than an 8B, or that this is
AGI, superintelligence, or open-ended reasoning. Those remain
**WITHHELD**.
