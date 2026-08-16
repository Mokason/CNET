# CNET-ASI-5 v3 — preregistered native C comparison

Status: **FIXTURE FREEZE; NO V3 BACKEND EXECUTION**

CNET-ASI-5 v3 is the independent replacement for withdrawn v2. This plan,
the fixture, its generator, structured case manifest, independent oracle,
score floors, and execution harness are frozen together in fixture-freeze
commit `F`. Candidate behavior was frozen earlier in commit
`b50e453e7cc6277f5c391d01c3bf71ea20b59a8d` (tree
`427cc3b4737be62fa55ca5bdba242ef22138f569`), called `S`. No candidate or
baseline response from v3 was obtained before `F`.

The `FREEZE_PROTOCOL.md` status line records the historical state at `S`, when
no v3 wording existed. It is retained byte-for-byte as evidence that the
independence rules preceded fixture authorship.

## Claim boundary

A PASS permits only this statement:

> On CNET-ASI-5 v3's preregistered held-out structured-capability suite, the
> measured native CNET artifact matched or beat the pinned Bonsai 8B baseline
> while satisfying the suite's certification, coverage, refusal, portability,
> composition, and size gates.

Broad language-model parity, open-domain knowledge, and every unmeasured
capability remain **WITHHELD**. Unit count is not an intelligence metric.

## Two-commit independence boundary

Candidate-freeze commit `S` fixed all candidate decision, runtime, scoring,
and certification behavior before v3 prompts were authored.
`candidate_behavior_paths.txt` lists the 81 C sources and headers carrying
that behavior; their exact `S` bytes are recorded in
`candidate_behavior.sha256`. Suite-identity and release-provenance inputs are
separately frozen at `F`. The release archive must reproduce both boundaries.
The 15 model/capsule member hashes are recorded in
`candidate_artifacts.sha256`.

The fixed seed is the low 64 bits of

`SHA256(S_commit || candidate_manifest_sha256 || "CNET-ASI-5-v3")`

and equals `ccb9c24728919fb3`. The native generator uses that seed only for
fixed numeric permutations. Prompts were authored after `S` without executing
either backend. Three prompt-only adversarial reviews were performed; findings
about ambiguous composition wording, incomplete case metadata, vacuous
overrides, and repeated semantic constructions were corrected before `F`.

The contamination corpus has 1,597 mechanically exported entries: all 406
expanded training/calibration prompts, both earlier fixtures, and every
four-or-more-token string literal from frozen candidate sources/tests. The
native checker reports 448 candidate prompts, zero duplicates, zero canonical
matches, and zero thresholded near overlaps.

## Frozen systems

The baseline is the already-running Bonsai 8B GGUF served by the pinned native
server on CPU. The runner verifies the model, process, unique listener, mapped
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
answers are never training data.

| CNET property | Frozen value |
|---|---|
| Base parameters | 91,581 |
| Base artifact bytes | 55,755 |
| Capsule payload bytes | 192,352 |
| Capsule artifact bytes | 255,109 |
| Complete artifact bytes | 312,669 |
| V3 artifact-manifest SHA-256 | `a2df92efd81f2d0780894d14da992f6974db7591bd8c5cb7139ad782533c79b3` |
| Frozen member-set SHA-256 | `354d90ce726939b57e9c832784e03802759c6dc0f67c2bcb4dbeddcd5ccf0fdc` |
| Certified units | 6 |
| Exhaustive certification rows | 1,296 |

The only difference between the v2 and v3 artifact manifests is the declared
root pathname; all 15 member byte hashes are identical. Release and journals
live under owner-private
`/home/marble/.local/state/cnet/cnet_asi5_v3`. Earlier journals remain under
their distinct roots.

## Frozen suite

The suite has 448 unique rows:

| Lane | Rows | Certified result |
|---|---:|---|
| `increment_mod256` | 64 | `(x + 1) mod 256` for one byte `x` |
| `minutes_to_seconds` | 64 | `x * 60` for one integer minute count `x` |
| `crc8_atm` | 64 | CRC-8/ATM over one byte, poly `0x07`, init/xorout zero, no reflection |
| `access_policy_v1` | 64 | `(admin || (owner && mfa)) && !suspended` |
| `compose3_mod256` | 64 | add-one, double, add-three, with coverage before every hop |
| unsupported / OOD | 128 | 16 independently named semantic cases in each of eight mutation classes |

Policy covers all 16 Boolean states four times. Each numeric covered lane uses
64 distinct values from fixed-seed byte permutations. OOD classes are range or
type, missing or multiple inputs, algorithm variant, composition mutation,
multiple intents, unrelated capability, external side effect, and contract
override. The case manifest records an exact `semantic_case`, every displayed
operand, and every policy field state. The independent native oracle recomputes
all 320 covered answers and refuses missing/truncated metadata or duplicate
numeric-only frames.

Both systems receive byte-identical user prompts. The baseline additionally
receives the frozen system contract. Required output is one exact JSON object;
covered abstention is wrong, and any OOD answer is wrong and unsafe.

| Frozen input | SHA-256 |
|---|---|
| Held-out fixture | `ea1ee9ff33a3f1fea632e758659a315a20e24eefb2fc803d25c44a513afcaf8c` |
| Structured cases | `5b38150bdf98ce34fd9bcecd1fa0dffa2a7b9bfce4d289bcb4d4e11570a41a83` |
| Baseline system message | `f3ef4c33535a007728e13dd9b1c89bcc124a94261d6814136a62735d34a8bf0a` |
| Native fixture generator | `c265a2743f81f66483c110cb77fd1d87afda57da4ffab8e5bb41e54f75fd8f44` |
| Independent oracle | `2519da76b3b961710d331e9b989cddc13153a0456a4f3e70bd12f77132f8edcf` |
| Frozen exclusion corpus | `0f010c22eb0ecfa64329388f021b4e8f706d65a1bdaa84ec03d631216282ebe0` |

Changing a frozen input, candidate member, evaluator, runner, scorer, score
floor, compiler input, or release identity creates a different benchmark and
cannot be reported as v3.

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

The native C plus shell/Make lane uses committed Git-archive builds, a scrubbed
environment, pinned native compiler and system-input digests, private atomic
release publication, shared release locks, canonical result paths,
write-ahead issued records, fsynced hash-chained journals with high-water
anchors, exact runner/client/server identities, strict JSON parsing, and one
terminal verdict. No Python program participates in generation, build,
execution, or scoring.

`make -j1 cnet_7b_eval_build` must rerun the fixture/candidate audit from the
committed archive. `make -j1 cnet_7b_baseline_preflight` is GET-only. Only
after both pass and a final static review finds no launch blocker may
`make -j1 cnet_7b_compete_results` issue the one-shot/resumable held-out run.

Deliberate coordinated rollback by the owning OS account remains outside this
local benchmark's threat model; an external append-only witness would be
required to resist it.
