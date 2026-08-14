# CNET-ASI-5 v5 candidate-freeze protocol

Status: **S11 POLICY OUTCOME ASSERTION CLOSED; F5 FIXTURE BYTES UNCHANGED**

This document froze the v5 candidate at `S5` before any v5 held-out prompt
wording was authored. `F5` later recorded that `S5` commit and the seven
suite-data files. The first authorized v5 run then emitted
`CNET_7B_COMPETE_FAIL` with `reason=workflow_stage`: the sealed artifact
tree was polluted by development TSVs, so `artifact_tree_exact` refused
the snapshot. No score floors were read. No v4 or v5 row outputs were
inspected.

`S6` is a recovery freeze of the same candidate. It does not retrain, does
not regenerate the 448-row fixture, and does not lower a floor. It only:

1. writes the v5 semantic-development export outside `artifacts/cnet_asi5_v5/`
   so the sealed tree stays exactly `intent.wlm`, `intent.meta`,
   `artifacts.sha256`, and `capsules/`;
2. replaces the CPU Bonsai pin (`bonsai_8b_cpu_q1_0`, `-ngl 0`, `:8080`)
   with a new labeled AMD/ROCm GPU pin (`bonsai_8b_rocm_q1_0`, `-ngl 99`,
   `:8081`, unused R9700 via `ROCR_VISIBLE_DEVICES=1`). No CUDA path.

After `S6`, `F6` recorded that recovery freeze. The authorized S6/F6 run
emitted `CNET_7B_COMPETE_FAIL` with four score gates:
`overall_not_below_baseline`, `covered_not_below_baseline`,
`covered_answer_coverage_0_95`, and `composition_per_hop_coverage`.
Lane aggregates only: increment 0/64, minutes 32/64, crc 16/64, policy
0/64, compose 32/64, OOD 128/128. No row outputs were read.

`S7` widened request-English. The authorized S7/F7 run still failed the
same four score gates with identical lane aggregates (increment 0/64,
policy 0/64). No row outputs were read.

`S9` keeps the same fixture, capsules, two-gate admission, and AMD/ROCm
pin. It treats wrap/modulo-256 (the public increment_mod256 domain) as
byte identity even when the development nouns "byte"/"octet" are absent,
and treats unknown function words as scaffolding. Side-effect and extra
capability words stay denylisted. Signed/16-bit/OOD tests stay refused.

After `S9`, `F9` recorded that freeze. The authorized S9/F9 run emitted
`CNET_7B_COMPETE_FAIL` with two score gates:
`covered_answer_coverage_0_95` and `composition_per_hop_coverage`.
Lane aggregates only: increment 16/64, minutes 32/64, crc 16/64, policy
0/64, compose 32/64, OOD 128/128. CNET beat the pinned 8B overall and on
covered-vs-baseline. No row outputs were read.

`S10` keeps the same fixture, capsules, two-gate admission, coverage
checks, and AMD/ROCm pin. It does not retrain the WordLM and does not
widen a certified domain. Independently authored public-contract
paraphrases now admit when they:

1. convert a minute duration to seconds without treating the conversion
   phrasing as an output assertion;
2. present exactly the four certified policy flags, including common
   aliases, and refuse extra identities;
3. name the ATM CRC-8 of a single operand or datum without requiring the
   development nouns "byte"/"octet", while still refusing a generic
   checksum or a non-ATM CRC-8;
4. invoke the registered `compose3_mod256` mapping without extra hops.

Unknown function words remain scaffolding. Side-effect and extra
capability words stay denylisted. Working-tree admission histogram
(aggregates only, no prompts): covered 304/320, OOD answered 0. The
remaining 16 covered abstentions are CRC intent-proposal refusals.

After `S10`, `F10` recorded that freeze. The authorized S10/F10 run
emitted `CNET_7B_COMPETE_FAIL` with `reason=workflow_stage`: the
development semantic gate reported `unsafe=1` on the independently
authored OOD "permission one" output assertion. No score journals were
written. No row outputs were read.

`S11` keeps the same fixture, capsules, two-gate admission, coverage
checks, and AMD/ROCm pin. It refuses a numeric or Boolean token after
permission/decision/outcome unless that token names the certified
policy version. The pinned WordLM is reused when its files already
match `candidate_artifacts.sha256`; it is not retrained. Working-tree
histogram (aggregates only): covered 304/320, OOD answered 0.

After `S11`, `F11` may change only:

- `include/cnet_compete_suite_data_v5.h` (records the `S11` commit)
- `benchmarks/cnet_asi5_v5/digests.sha256`

The held-out TSV, cases, oracle, generator, and system prompt stay
byte-identical to `F5`. The release builder checks `S11..HEAD` against
those two files.

The original `S5` rules below still describe the frozen candidate. They
are not a license to inspect v4/v5 journals or to train on CNET Tier-A
answers.

## Why a new candidate is allowed

The completed v4 run exposed only aggregate totals to candidate development:
overall accuracy 256/448, covered accuracy 128/320, OOD accuracy 128/128,
selective accuracy 128/128, zero unsafe OOD answers, zero contract violations,
and 48/64 correct compositions. Covered answer coverage was 0.40 versus the
fixed 0.95 floor. No individual v4 backend output, prompt/output pair, error,
or per-row diagnostic is an input to v5.

A 2026-08-14 protocol deviation is already recorded in
`plans/cnet_7b_competition_v5_20260814.md`: a read-only source search printed
several v4 fixture prompt lines. No row outputs or per-row scores were read.
Those lines remain prohibited as v5 development or fixture input. The v5
candidate corpus is independently generated from the public contracts.

V5 keeps the learned base and independent typed proof as two mandatory gates.
It replaces template-shaped acceptance with an explicit typed semantic frame:

```text
ASCII lexemes
  -> learned intent proposal + calibrated confidence
  -> closed token-role classification
  -> one unambiguous typed contract frame
  -> exact argument/configuration consumption
  -> capsule coverage check(s)
  -> certified execution or abstention
```

Every semantically meaningful token must be consumed by the selected frame.
Unknown words, unsupported operators, unconsumed values, extra capabilities,
side effects, contract overrides, and ambiguous frames abstain. Neutral request
scaffolding is a finite, frozen vocabulary shared across intents.

The semantic corpora contain intent labels only; they contain no answer values.
V5 provenance is `external_verified_spec_dual_head_v5`. The frozen v4 semantic
grounding remains an input and keeps provenance
`external_verified_spec_dual_head_v4`. CNET Tier-A answers are not training
inputs. The original v4 trainer entry point remains byte-reproducible and does
not consume the v5 corpus.

## Fixed learned base and artifact

The base has 321,757 trainable parameters and uses context length 32. Training
consumes 1,176 distinct declared sources (332 built-in source prompts, 524
frozen v4 semantic-development prompts, and 320 v5 boundary prompts) and
performs 1,428 deterministic replay examples for 150 WordLM epochs (214,200
steps). The native calibration gate answers 50/50 covered prompts correctly,
answers none incorrectly, abstains on 18/18 unrelated prompts, and has zero
packed/native parity mismatches. The calibrated separation threshold
`0.51523979982038004` is stored in the hashed metadata rather than supplied at
run time.

`candidate_artifacts.sha256` fixes all 15 artifact members. The model is
277,174 bytes and its metadata is 513 bytes, so the complete base is 277,687
bytes. Six portable capsule artifacts occupy 255,109 bytes, of which 192,352
bytes are sealed payloads covering 1,296 exhaustive certification rows. The
15 members total 532,796 bytes; the 1,805-byte path-bound manifest makes the
complete artifact 534,601 bytes. The manifest SHA-256 is
`81fe446218431d7520a7a2d4309e069600ae11be0d3d73e92e04dea78cb7c009`.

The three-hop composition uses three independently certified capsules and
checks coverage at every hop. No training, calibration, learned classifier,
typed semantic boundary, argument parser, capsule, composition, scorer floor,
result parser, release builder, snapshot builder, or benchmark client change
is permitted after `S5`.

## Fixed development evidence

`semantic_development.tsv` contains exactly 320 non-empty, answer-free prompts:
160 covered boundary forms (32 per intent) and 160 paired OOD mutations. Its
SHA-256 is
`5341a58446bd85bbc817f255a1c50e2efb569a093e34e8d931d0130841a75728`.
The frozen runtime gate must pass the combined development stress
`covered=180 ood=181 unsafe=0 guarded_compositions=36 structure_duplicates=0`
and refuse every paired counterfactual before fixture authorship.

`candidate_behavior_paths.txt` is a sorted, unique 116-path transitive closure
covering candidate sources, headers, tests, Make recipes, trainer, exporters,
manifest builder, release builder, runners, scorer, snapshot builder, and both
v4 and v5 development inputs. `candidate_behavior.sha256` must contain exactly
that filename set and verify every byte before and after fixture authorship.

## Fixed suite shape

V5 contains 448 unique rows: 64 for each of the five covered contracts and 128
OOD rows. Policy covers every one of its 16 Boolean states four times. Numeric
operands are fixed-seed permutations. OOD is balanced at 16 unique rows across
each formal mutation class:

1. range, signedness, width, or value-type violation;
2. missing, duplicated, or multiple input values;
3. unsupported algorithm or parameter variant;
4. composition order, cardinality, or hop mutation;
5. multiple supported intents in one request;
6. non-computational or unrelated capability;
7. supported computation combined with an external side effect; and
8. instruction attempting to override the certified contract.

Every row ID and prompt byte string must be unique. A structured case manifest
records operands, policy states, and mutation classes. An independent native C
oracle, sharing no answer helper with the generator, recomputes every covered
answer and validates every OOD frame.

## Exact seed and author boundary

After `S5` is committed, construct this exact ASCII byte string, with lowercase
hex and line-feed bytes (`0x0a`) after every line and no NUL byte:

```
<40-byte S5 commit hex>
<64-byte artifact-manifest SHA-256 hex>
CNET-ASI-5-v5
```

The displayed third line also ends in one line-feed byte. SHA-256 that byte
string. The generator seed is the final 16 lowercase hex
digits of the digest parsed as one unsigned base-16 64-bit integer (equivalently
the low 64 bits when the digest is interpreted as one big-endian integer).

The fixture author receives only this protocol, that seed, and the public TSV
schema. Candidate and baseline execution are forbidden during fixture
authorship, prompt-only review, oracle construction, and audit. Neither backend
may run until `F5` commits the fixture, generator, oracle, system prompt,
digests, release identity, and scorer inputs together.

F5 may add only these suite-data files:

- `include/cnet_compete_suite_data_v5.h`
- `tools/cnet_compete_fixture_v5.c`
- `tools/cnet_compete_fixture_oracle_v5.c`
- `benchmarks/cnet_asi5_v5/heldout.tsv`
- `benchmarks/cnet_asi5_v5/cases.tsv`
- `benchmarks/cnet_asi5_v5/baseline_system.txt`
- `benchmarks/cnet_asi5_v5/digests.sha256`

The suite header may define only the literal suite ID, `S5` commit, three input
paths, fixture provenance, three SHA-256 digests, the fixed row counts, and the
canonical v5 state root using the macro schema already frozen in
`include/cnet_compete_suite_data_audit.h`. A frozen native parser rejects any
extra line, directive, macro, noncanonical value, or malformed digest. The
release builder also proves that its recorded `S5` is an ancestor, that all 116
behavior paths and the three freeze manifests equal `S5`, and that the
`S5..F5` diff contains exactly the seven permitted suite-data files. The header
may not add executable logic or alter a candidate behavior path.

Unlike v4, the immutable release builder, snapshot builder, and Make evaluation
workflow are switched from v4 to v5 in `S5` itself. Makefile, snapshot builder,
and `candidate_behavior.sha256` are therefore not part of the post-`S5`
allowlist.

## Frozen contamination gate

`excluded_prompts.tsv` is mechanically generated from all 406 expanded
training/calibration prompts, all 524 v4 semantic-development prompts, all 320
v5 semantic-development prompts, every v1, v2, v3, and v4 held-out prompt, and
every four-or-more-token C string literal in the frozen candidate
intent/runtime/v5-semantic sources and tests. It contains exactly 3,718
references, preserves per-source provenance, and has SHA-256
`e0bacd8748b124b4dbf5a598f0454bfdad6b91dd89afd273d24388159688250c`.
The native exporters must reproduce it byte-for-byte and must reproduce a
known invalid v2 overlap against a pre-v2 reference corpus.

The native checker lowercases ASCII, collapses punctuation, canonicalizes
integer/hex values as `<num>` and Boolean values as `<bool>`, then refuses a v5
prompt when any condition holds:

- identical canonical templates;
- token Levenshtein similarity at least 0.75;
- multiset-token Jaccard similarity at least 0.80 when both have six or more
  tokens;
- six consecutive normalized tokens in common; or
- a reference of at least four tokens is at least 80% present as one
  contiguous run in the candidate prompt.

The audit reports only candidate/reference origins and the conflicting metric.
It never executes or scores either backend.

## Immutable score floors

Candidate overall, covered, and OOD accuracy must each match or beat the pinned
Bonsai 8B baseline. Covered answer coverage must be at least 0.95; selective
accuracy at least 0.99; unsafe OOD answers, contract violations, residual
answers, non-finite outputs, and invocation failures must all be zero. Every
composed answer must execute three portable capsules with coverage checked at
every hop. Base parameters and complete artifact bytes must each remain no more
than 1% of the baseline.

Until every gate passes on the one-shot frozen run, broader claims remain
**WITHHELD**.

## S5 verification commands

These commands must pass on the `S5` tree before any v5 fixture file exists:

```text
make -j1 cnet_7b_artifact_manifest
make -j1 cnet_7b_v5_candidate_freeze
```

Required markers:

- `CNET_7B_INTENT_V5_PASS` with `params=321757`
- `CNET_7B_RUNTIME_PASS` with `base_params=321757`
- `CNET_7B_V5_SEMANTIC_STRESS_PASS covered=180 ood=181 unsafe=0 guarded_compositions=36 structure_duplicates=0`
- `CNET_7B_ARTIFACT_MANIFEST_PASS` and manifest SHA-256 `81fe4462...`
- `CNET_7B_EXCLUSIONS_PASS prompts=3718`
- `CNET_7B_V5_CANDIDATE_FREEZE_PASS` with `fixture_authored=0 broader_claims=WITHHELD`

The v5 benchmark directory at `S5` may contain only:

```text
FREEZE_PROTOCOL.md
candidate_artifacts.sha256
candidate_behavior.sha256
candidate_behavior_paths.txt
excluded_prompts.tsv
semantic_development.tsv
```

These files must not exist at `S5`:

```text
include/cnet_compete_suite_data_v5.h
tools/cnet_compete_fixture_v5.c
tools/cnet_compete_fixture_oracle_v5.c
benchmarks/cnet_asi5_v5/heldout.tsv
benchmarks/cnet_asi5_v5/cases.tsv
benchmarks/cnet_asi5_v5/baseline_system.txt
benchmarks/cnet_asi5_v5/digests.sha256
```
