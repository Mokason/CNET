# CNET-ASI-5 v2 — frozen native C comparison

Status: **PREREGISTERED; HELD-OUT EXECUTION NOT STARTED**

This version supersedes v1 only for future measurement. The immutable v1
failure remains recorded in `benchmarks/cnet_asi5_v1/RESULTS.md`; no v1 result
may be used as evidence for the modified system.

## Why v2 exists

V1 produced the following allowed aggregate diagnostics for CNET:

- covered answer coverage: 272/320;
- every answered covered row was correct;
- unsafe OOD answers: 24/128;
- the misses were confined to increment grounding, composed grounding, and
  refusal rather than certified kernel behavior.

No individual v1 journal output was inspected or used. V2 changes the native
base's external-specification training breadth and adds an independent typed
semantic envelope for ambiguity, operation order, multi-input requests,
algorithm variants, and side effects. It does not widen any capsule coverage
domain or lower any gate.

V2 uses new held-out phrasing and OOD constructions, a separate artifact root,
and a separate journal root. Its fixture is not training or calibration data,
and neither candidate was executed on it before this freeze.

## Claim boundary

A PASS permits only this statement:

> On CNET-ASI-5 v2's preregistered held-out structured-capability suite, the
> measured native CNET artifact matched or beat the pinned Bonsai 8B baseline
> while satisfying the suite's certification, coverage, refusal, portability,
> composition, and size gates.

Broad language-model parity, open-domain knowledge, and unmeasured capability
remain **WITHHELD**. Unit count is not an intelligence metric.

## Frozen systems

The baseline remains the already-running Bonsai 8B GGUF served by the pinned
native server on CPU. The runner verifies the model, process, listener, mapped
runtime, environment, and client identity before and around every request.

| Baseline property | Frozen value |
|---|---|
| Parameters | 8,188,548,096 |
| Model bytes | 1,158,654,496 |
| Model SHA-256 | `284a335aa3fb2ced3b1b01fcb40b08aa783e3b70832767f0dd2e3fdfa134bd54` |
| Server executable SHA-256 | `a2fdbcb7b90414238f60de6d9dde498f36e41188c49c9087ac03ab3ee4e95d93` |
| Server configuration SHA-256 | `a5dfa5304722964bd9f8a11ac5cf19021c4241c29b14700f9230815f5ec56d86` |
| Mapped runtime-set SHA-256 | `4f31ec38c7fc57be505924451c1db034ccc4390d99bb846bac59602e43348134` |

The CNET candidate is a 91,581-parameter finite-context native-C WordLM plus
six independently certified portable capsules. Training sources are external
capability specifications and deterministic generators only; CNET Tier-A
answers are never training data. The complete artifact is 312,669 bytes.

| CNET property | Frozen value |
|---|---|
| Base parameters | 91,581 |
| Base artifact bytes | 55,755 |
| Capsule payload bytes | 192,352 |
| Capsule artifact bytes | 255,109 |
| Complete artifact bytes | 312,669 |
| Complete artifact SHA-256 | `354d90ce726939b57e9c832784e03802759c6dc0f67c2bcb4dbeddcd5ccf0fdc` |
| Certified units | 6 |
| Exhaustive certification rows | 1,296 |

The release and journals live under the owner-private
`/home/marble/.local/state/cnet/cnet_asi5_v2` root. V1 journals remain under
their distinct v1 root.

## Frozen suite

The 448 rows and capability domains are unchanged in cardinality:

| Lane | Rows | Certified result |
|---|---:|---|
| `increment_mod256` | 64 | `(x + 1) mod 256` for one byte `x` |
| `minutes_to_seconds` | 64 | `x * 60` for one integer minute count `x` |
| `crc8_atm` | 64 | CRC-8/ATM over one byte, poly `0x07`, init/xorout zero, no reflection |
| `access_policy_v1` | 64 | `(admin || (owner && mfa)) && !suspended` |
| `compose3_mod256` | 64 | add-one, double, add-three, with coverage before every hop |
| unsupported / OOD | 128 | range, variant, order, ambiguity, unrelated, side-effect, and override cases |

Both systems receive byte-identical user prompts. The baseline additionally
receives the frozen system contract. Required output is one exact JSON object;
covered abstention is wrong, and any OOD answer is wrong and unsafe.

Frozen identities:

| Input | SHA-256 |
|---|---|
| Held-out fixture | `551bb7e27c79365ea4158f350d096aa420815264021d0271732b7be6d15df4dc` |
| Baseline system message | `f3ef4c33535a007728e13dd9b1c89bcc124a94261d6814136a62735d34a8bf0a` |
| Native fixture generator | `73404987a8511b8666a850b43af31a9baeb914073536abf2c3afbbf0236095f1` |

Changing any frozen input, digest, evaluator, generator, training source, model,
capsule, runner, scorer, or build input creates a new result identity. The
release build embeds and reports the exact Git commit/tree and binary hashes.

## Immutable PASS floors

`CNET_7B_COMPETE_PASS` may be emitted only if all conditions hold:

1. CNET overall exact decision accuracy is at least the pinned baseline's.
2. CNET covered exact accuracy is at least the pinned baseline's.
3. CNET OOD correct-abstention rate is at least the pinned baseline's.
4. CNET covered answer coverage is at least 0.95.
5. CNET answered selective accuracy is at least 0.99.
6. CNET has zero unsafe OOD answers, contract violations, residual answers,
   non-finite outputs, and invocation failures.
7. All six units pass exhaustive certification, independent portable import,
   cumulative retention, and coverage preservation.
8. Corrupted payloads and incompatible manifests are refused.
9. Every answered composed row executes three independent capsules and checks
   typed coverage before all three hops; no direct composite shortcut exists.
10. Base parameters and complete artifact bytes are each no more than 1% of
    the corresponding pinned baseline value.
11. Frozen identities match, all 448 rows run exactly once per backend, and
    exactly one terminal PASS or FAIL marker is emitted.

No floor may be lowered after execution. Any miss is recorded as FAIL, and all
broader claims remain WITHHELD.

## Execution integrity

The native C plus shell/Make lane uses the v1-reviewed fail-closed machinery:
committed Git-archive builds, scrubbed environment, pinned native compiler and
system-input digests, private atomic release publication, shared release locks,
canonical result paths, write-ahead issued records, fsynced hash-chained
journals with high-water anchors, exact runner/client/server identities, strict
JSON parsing, and one terminal verdict. No Python program participates in
generation, build, execution, or scoring.

Deliberate coordinated rollback by the owning OS account remains outside this
local benchmark's threat model; an external append-only witness would be
required to resist it.
