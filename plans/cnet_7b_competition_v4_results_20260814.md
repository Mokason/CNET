# CNET-ASI-5 v4 frozen benchmark result

Status: **TERMINAL FAIL — TWO PREREGISTERED GATES MISSED**

On 2026-08-14, commit `4d2a5a12213bc569c583b23ed24af2a6f204d1c5`
(tree `db15161ecf67ad7c2db7936ec30260834a7c37e7`) completed the
one-command native benchmark:

```text
make -j1 cnet_7b_compete_results
```

The run issued all 448 frozen rows once to each backend and emitted exactly one
terminal marker:

```text
CNET_7B_COMPETE_FAIL suite=CNET-ASI-5-v4 failed_gates=2 broader_claims=WITHHELD
```

This is useful negative and comparative evidence, not a benchmark PASS. The
certification floors are unchanged and broader claims remain **WITHHELD**.

## Aggregate result

| Metric | CNET native C | Bonsai 8B CPU | Outcome |
|---|---:|---:|---|
| Overall exact decisions | 256/448 (0.571428571) | 200/448 (0.446428571) | CNET +12.5 percentage points |
| Covered exact decisions | 128/320 (0.400000000) | 74/320 (0.231250000) | CNET +16.875 percentage points |
| Covered answer coverage | 128/320 (0.400000000) | 118/320 (0.368750000) | CNET missed the fixed 0.95 floor |
| Selective accuracy | 128/128 (1.000000000) | 74/119 (0.621848739) | CNET passed the fixed 0.99 floor |
| OOD correct abstention | 128/128 (1.000000000) | 126/128 (0.984375000) | CNET passed |
| Unsafe OOD answers | 0 | 2 | CNET passed zero-unsafe floor |
| Contract violations | 0 | 6 | CNET passed zero-violation floor |
| Invocation failures | 0 | 0 | Both clean |
| Median latency | 0.296016 ms | 527.264001 ms | CNET 1,781.201x faster |
| p95 latency | 0.471265 ms | 894.579968 ms | CNET 1,898.253x faster |

The baseline latency is explicitly CPU-bound and is not a GPU-serving claim.

## Per-lane aggregate result

| Lane | CNET | Bonsai 8B |
|---|---:|---:|
| `increment_mod256` | 32/64 | 12/64 |
| `minutes_to_seconds` | 16/64 | 58/64 |
| `crc8_atm` | 16/64 | 0/64 |
| `access_policy_v1` | 16/64 | 3/64 |
| `compose3_mod256` | 48/64 | 1/64 |
| unsupported / OOD | 128/128 | 126/128 |

Every CNET covered answer was correct. Its 192 wrong decisions were covered
abstentions, not wrong substantive answers. No individual v4 backend response,
prompt/output pair, or per-row diagnostic is an input to subsequent candidate
development.

## Failed gates

1. `covered_answer_coverage_0_95`: CNET answered 128/320 covered rows (0.40),
   below the immutable 0.95 floor.
2. `composition_per_hop_coverage`: CNET answered 48/64 composed rows and made
   all three required coverage checks on each answered row (144 checks). The
   remaining 16 composed rows abstained, so the suite-wide three-hop coverage
   requirement was not met.

The six portable capsules themselves remained green: 1,296 exhaustive
certification rows, 256 development compositions, 768 development per-hop
coverage checks, and explicit corruption and incompatibility refusal.

## Size evidence

| Property | CNET | Bonsai 8B | Ratio |
|---|---:|---:|---:|
| Learned base parameters | 272,605 | 8,188,548,096 | 0.000033291 (30,038x smaller) |
| Complete artifact bytes | 524,745 | 1,158,654,496 | 0.000452892 (2,208x smaller) |
| Base artifact bytes | 267,831 | — | — |
| Capsule artifact bytes | 255,109 | — | — |
| Sealed capsule payload bytes | 192,352 | — | — |

Artifact manifest SHA-256:
`df0f7131aaa75627d5c542b375a39615ab16b93388878490037ad76b79a2d661`.

## Preserved evidence

| Evidence | SHA-256 |
|---|---|
| `logs/cnet_7b_compete_results.log` | `bdcc49b425c363aa41c4792af737ad2f22e6ae1160e06a957f7acb2051765852` |
| `logs/cnet_7b_eval_build.log` | `ca310e46a492e249d7e91df1e70a179b2bcc094ab3167dd9e34dc99799e586e8` |
| `logs/cnet_7b_baseline_run.log` | `d823c1fbe9a2feae9d10db0127394c85374d56c21f1008157b40dbed026e51f2` |
| `logs/cnet_7b_cnet_run.log` | `b9295a28951ce5dc3803812d039f2c5536ec6a1c972e2c0a01d89b84b8427379` |

Canonical journals and anchors are owner-private under
`/home/marble/.local/state/cnet/cnet_asi5_v4/results/`.

## Freeze-protocol disclosure

Candidate-freeze commit `S4` is
`c2dfafe068ad0589d4af8a6f6cbca8d1462829d4`; fixture-freeze commit `F4` is
`80cd601295a8ac904b56da58bf38f9a4402565eb`. Before any held-out request was
issued, three build-only errata were committed:

- `cd833228ad5332f0248649a156d5e6a7fc3c2b9f`: repair nested shell quoting;
- `678b8cacad6b8392f09d6a5b6c84f4dad6f9207e`: pin the manifest to Git-archive
  line-ending bytes without changing either source blob;
- `4d2a5a12213bc569c583b23ed24af2a6f204d1c5`: preserve the suite-header macro
  through nested Make and add an early syntax-only smoke gate.

These changes touched release/build infrastructure and its integrity manifest,
not the learned candidate, runtime semantics, fixture, oracle, scorer, or any
floor. They are nevertheless an explicit deviation from the strict two-commit
story and are recorded rather than hidden. The terminal result is FAIL in any
case.
