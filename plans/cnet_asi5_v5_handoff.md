# CNET-ASI-5 v5 handoff

Updated: 2026-08-14 (S10: public-contract paraphrase admission)

## Goal and completion rule

Produce one tangible, authenticated native C/C++ benchmark comparing CNET ASI
with the pinned Bonsai 8B baseline. The goal is complete only when the frozen
one-command gate emits exactly one terminal `CNET_7B_COMPETE_PASS` and reports
the benchmark metrics. Keep broader claims `WITHHELD` until then.

Non-negotiables: no Python in the lane; AMD/ROCm only; never lower
certification floors; never train on CNET Tier-A answers; do not inspect
individual v4 prompts or outputs; do not create a second packaging system.

## What "improve" means from this checkpoint

Improve the freeze/handoff operational quality so the next session can finish
without rediscovering protocol. Do **not** reopen candidate design, grow the
model, widen a certified domain, or touch v4 journals.

The v5 development candidate already reports 180/180 covered and 181/181 OOD
on the answer-free stress matrix. Held-out coverage can be measured only after
`S5` then `F5`. A FAIL starts a new independent candidate cycle.

## Current state

- Branch: `feature/cnet-7b-competition`.
- `S5` `0abf82bc73e05067bb15268b20a60b7c64968ba7`, `F5`
  `3e7c9211a9568066ac2ea685736853dc6ad0f35e`.
- Official `make -j1 cnet_7b_compete_results` after `F5` emitted
  `CNET_7B_COMPETE_FAIL suite=CNET-ASI-5-v5 failed_gates=1
  reason=workflow_stage broader_claims=WITHHELD`.
- Root cause: `cnet_7b_v5_semantic_corpus` wrote
  `artifacts/cnet_asi5_v5/semantic_development.tsv` into the sealed tree.
  `artifact_tree_exact` refused the extra file. Floors were not scored.
  No v4/v5 row outputs were read.
- `S6`/`F6` official run: `CNET_7B_COMPETE_FAIL` four score gates.
  Bonsai ROCm 214/448 covered 87/320; CNET 208/448 covered 80/320
  (increment 0, policy 0). Journals stay in `cnet_asi5_v5`. No rows read.
- `S7`/`F7` official run: same four score gates as `S6`, same lane
  aggregates. Journals archived as `cnet_asi5_v5_s7f7_fail`.
- `S9`/`F9` official run: `CNET_7B_COMPETE_FAIL` two score gates
  (`covered_answer_coverage_0_95`, `composition_per_hop_coverage`).
  Bonsai ROCm 214/448 covered 87/320; CNET 224/448 covered 96/320
  (increment 16, minutes 32, crc 16, policy 0, compose 32, OOD 128).
  No rows read. Archive those journals as `cnet_asi5_v5_s9f9_fail`
  before the next authorized run.
- `S10` (this tree): keep two-gate admission, capsules, coverage, and
  the AMD/ROCm pin. Admit independently authored public-contract
  paraphrases (minutes conversion phrasing, four-flag policy, ATM
  checksum of an operand/datum, registered compose3). Generic checksum
  and non-ATM CRC-8 still abstain. WordLM not retrained. Working-tree
  histogram: covered 304/320, OOD 0. `CNET_7B_V6_COHERENCE_PASS
  covered=25 ood=7`. Runtime matrix still green. Same ROCm pin PID
  `626910` on `:8081`.
- `F10` after `S10` only retargets the suite header at the `S10` hash
  and refreshes `digests.sha256`. Same F5 fixture.
- Then one authorized `make -j1 cnet_7b_compete_results`. Never start a
  second result path. Floors unchanged. Broader claims `WITHHELD`.

## Verified v5 development result

| Item | Value |
|---|---|
| Learned base | 321,757 parameters, context 32 |
| Training profile | 1,176 sources, 1,428 replay examples, 214,200 steps |
| Calibration | 50/50 covered correct, 18/18 OOD abstain, 0 wrong, 0 parity mismatch |
| Stored threshold | `0.51523979982038004` |
| Provenance | `external_verified_spec_dual_head_v5` |
| Base artifact | 277,687 bytes (`intent.wlm` 277,174 + `intent.meta` 513) |
| Six portable capsules | 255,109 bytes total, 192,352 sealed, 1,296 certified rows |
| Complete 15-member artifact + manifest | 534,601 bytes |
| Artifact-manifest SHA-256 | `81fe446218431d7520a7a2d4309e069600ae11be0d3d73e92e04dea78cb7c009` |
| Model SHA-256 | `2a4328ff8900efc9ae2cc8c14c206043b25f428210c35d31bc833fe69a70fa1c` |
| Metadata SHA-256 | `a8259a7dddb8c94d0f95ff4aaa8e37907f8bbf26f53d194777b53e7ca9c2f393` |
| Semantic corpus SHA-256 | `5341a58446bd85bbc817f255a1c50e2efb569a093e34e8d931d0130841a75728` |
| Exclusion corpus SHA-256 | `e0bacd8748b124b4dbf5a598f0454bfdad6b91dd89afd273d24388159688250c` |
| Final answer-free stress | `covered=180 ood=181 unsafe=0 guarded_compositions=36 structure_duplicates=0` |

V4 model reproduction remains byte-identical, so v5 did not rewrite v4.

## Frozen-input work already prepared

Expected intentional changes for `S5`:

- `Makefile` — v5 capsule/runtime targets, artifact-manifest root, then
  candidate-freeze gate plus eval/release switch
- `include/cnet_compete_artifacts.h` — root, params, bytes, manifest digest
- `tools/cnet_compete_export_exclusions.c` — `--v5` exporter
- `tools/cnet_compete_snapshot_build.sh` — must switch v4 → v5 before `S5`
- new `benchmarks/cnet_asi5_v5/candidate_artifacts.sha256`
- new `benchmarks/cnet_asi5_v5/excluded_prompts.tsv`
- new `benchmarks/cnet_asi5_v5/FREEZE_PROTOCOL.md`
- new `benchmarks/cnet_asi5_v5/candidate_behavior_paths.txt`
- new `benchmarks/cnet_asi5_v5/candidate_behavior.sha256`
- this handoff file (not part of the frozen candidate closure)

The v5 exclusion corpus reproduced byte-for-byte:

- 3,718 reference rows (3,720 lines including its two-line preamble).
- SHA-256:
  `e0bacd8748b124b4dbf5a598f0454bfdad6b91dd89afd273d24388159688250c`.
- Export marker: `CNET_7B_EXCLUSIONS_PASS prompts=3718`.

Unrelated user-owned untracked paths must remain untouched:

- `result/`
- `third_party/`
- `soul_gemma4v2_final.cnb.gaps.txt.bak-pre-w16-park-20260802175113`

Do not touch `/home/marble/.local/state/cnet/cnet_asi5_v4`. That tree holds
the authenticated v4 journals.

## Last completed benchmark (v4, aggregate evidence only)

V4 terminal verdict was FAIL because two score gates were missed. Preserve its
448-row journals and do not inspect individual v4 prompts or outputs.

- Bonsai 8B: overall 200/448 (0.4464), covered 74/320 (0.2313), OOD 126/128
  (0.9844), selective 74/119 (0.6218), 2 unsafe OOD answers, 6 contract
  violations, median 527.264 ms, p95 894.580 ms.
- CNET v4: overall 256/448 (0.5714), covered 128/320 (0.4000), OOD 128/128
  (1.0000), selective 128/128 (1.0000), zero unsafe answers/violations,
  median 0.296 ms, p95 0.471 ms.
- CNET beat the 8B baseline overall and on safety/latency, but its covered
  answer coverage was only 0.40 versus the fixed 0.95 floor. V5 exists to
  improve semantic coverage without widening certified domains.

## Resume order

1. Read root `AGENTS.md` and this file. Use codebase-memory project
   `home-marble-AI-CNET`. If that transport is closed, say so and use targeted
   `rg`.
2. Run `git status --short`; preserve the three unrelated paths above.
3. Done: `make -j1 cnet_7b_artifact_manifest` reproduced the table above.
4. Done as a working tree: v5 freeze protocol, 116-path closure, release
   builder switch, and `make -j1 cnet_7b_v5_candidate_freeze`. Commit this
   tree as `S5` before any fixture file exists. Remaining freeze checklist:
   - keep `benchmarks/cnet_asi5_v5/FREEZE_PROTOCOL.md`;
   - freeze the 116-path transitive behavior/build closure and hashes;
   - reproduce the model, semantic corpus, exclusion corpus, capsule audit,
     sanitizer gates, artifact manifest, and prior invalid-overlap regression;
   - prove that no v5 fixture/header/oracle/system/digest files exist;
   - switch the immutable release builder and Make evaluation workflow from v4
     to v5 **before** the candidate-freeze commit;
   - run `make -j1 cnet_7b_v5_candidate_freeze` and commit `S5`.
5. Only after `S5`, derive the documented deterministic seed and independently
   author exactly seven v5 suite-data files. Do not execute either backend while
   authoring. Run only generator/oracle/independence/data audits, then commit
   `F5`.
6. Build the immutable release from the clean Git archive and run the single
   authorized v5 benchmark. Resume journals on interruption; never start a
   second result path.
7. Report the exact terminal verdict and aggregate metrics. Mark the goal done
   only for authenticated `CNET_7B_COMPETE_PASS`.

## S5 file allowlist

`benchmarks/cnet_asi5_v5/` at freeze may contain only:

```text
FREEZE_PROTOCOL.md
candidate_artifacts.sha256
candidate_behavior.sha256
candidate_behavior_paths.txt
excluded_prompts.tsv
semantic_development.tsv
```

These files must not exist until `F5`:

```text
include/cnet_compete_suite_data_v5.h
tools/cnet_compete_fixture_v5.c
tools/cnet_compete_fixture_oracle_v5.c
benchmarks/cnet_asi5_v5/heldout.tsv
benchmarks/cnet_asi5_v5/cases.tsv
benchmarks/cnet_asi5_v5/baseline_system.txt
benchmarks/cnet_asi5_v5/digests.sha256
```

## Post-S5 allowlist

Exactly these seven files, and nothing else:

1. `include/cnet_compete_suite_data_v5.h`
2. `tools/cnet_compete_fixture_v5.c`
3. `tools/cnet_compete_fixture_oracle_v5.c`
4. `benchmarks/cnet_asi5_v5/heldout.tsv`
5. `benchmarks/cnet_asi5_v5/cases.tsv`
6. `benchmarks/cnet_asi5_v5/baseline_system.txt`
7. `benchmarks/cnet_asi5_v5/digests.sha256`

Makefile, snapshot builder, scorer, runtime, and
`candidate_behavior.sha256` must not change after `S5`. That is why the
release builder must already point at v5 in the freeze commit.

## Seed construction (fill after S5)

After `S5` is committed, SHA-256 this exact ASCII byte string. Each line ends
in `0x0a`. No NUL.

```text
<40-byte S5 commit hex>
81fe446218431d7520a7a2d4309e069600ae11be0d3d73e92e04dea78cb7c009
CNET-ASI-5-v5
```

The generator seed is the final 16 lowercase hex digits of that digest, parsed
as one unsigned base-16 64-bit integer.

Do not start fixture wording until that seed is written here.

## Important implementation boundaries

- Reuse the existing capsule/export mechanism; do not create another packaging
  system.
- The candidate closure must include the Makefile, snapshot builder, trainer,
  artifact/suite/eval headers, exporters, runtime, parser, runners, scorer,
  tests, and all v5 development inputs, including frozen v4
  `semantic_development.tsv`.
- Keep exact-once journals, private release paths, release locking, immutable
  archive builds, client/server identity checks, and exactly-one terminal
  verdict behavior intact.
- New private release root is `/home/marble/.local/state/cnet/cnet_asi5_v5`.
- Leave the v4 release root and its 448-row journals untouched.

## Exact commands

| Stage | Command | Required marker |
|---|---|---|
| Reproduce artifacts | `make -j1 cnet_7b_artifact_manifest` | `CNET_7B_V5_ARTIFACT_FREEZE_PASS members=15` |
| Candidate freeze | `make -j1 cnet_7b_v5_candidate_freeze` | `CNET_7B_V5_CANDIDATE_FREEZE_PASS ... fixture_authored=0` |
| After F5 only | `make -j1 cnet_7b_v5_fixture_audit` | `CNET_7B_V5_FIXTURE_AUDIT_PASS rows=448 ... overlap=0` |
| After F5 only | `make -j1 cnet_7b_eval_build` | exactly one `CNET_7B_EVAL_BUILD_PASS` |
| After F5 only | `make -j1 cnet_7b_compete_results` | exactly one `CNET_7B_COMPETE_PASS` or `FAIL` |

## Time estimate from this checkpoint

Approximately 4--6 focused hours if the freeze/release audit stays green:
1--2 hours to finish `S5`, 1--2 hours for independent fixture `F5`, and
0.5--1 hour for the final build/run/score, plus contingency for one
fail-closed repair.
