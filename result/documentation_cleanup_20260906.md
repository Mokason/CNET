# Documentation cleanup — September 6, 2026

Completed on `experiment/offline-controller-20260906`, from source checkpoint
`c6154036880c87db69c880ba36a22087815b48b0`. This is a documentation migration,
not deployment, a new model benchmark or security release clearance.

## Inventory and recovery

The [per-path inventory](../docs/DOCUMENTATION_INVENTORY.tsv) covers all 510
original tracked Markdown/text files (394 Markdown, 116 text):

| Disposition | Count | Scope |
| --- | ---: | --- |
| Rewritten | 96 | Current guides and stale navigation/status surfaces |
| Retired | 47 | 44 superseded plans/handoffs and three obsolete text files |
| Preserved byte-for-byte | 366 | Policies, data, inputs, decisions and evidence |
| Reference-only update | 1 | Two recovery links in a historical throughput spec |

Four new guides bring the rewritten/current guide set to 100: build/test,
GPU training, security and maintenance. This report, the cleanup plan and
inventory/archive metadata are additional records, not part of the original
510-file denominator.

[The checked-in archive](../attic/documentation-20260906.tar.gz) contains
144 exact original files (47 retired + 96 rewritten + one reference-updated).
Size: 679,555 bytes. SHA256:
`19f152004f3fa557410f869a3198c9fc6a3d7f7694e1eb8581014d0f6941c579`.
Every member was checked against the inventory and pre-edit original.
The [recovery guide](../docs/MAINTENANCE.md) includes the full external backup
and a tested single-member extraction into a new private directory.

Historical tutorial detail, benchmark tables, failed optimization results and
deferred implementation amendments remain recoverable. Frozen protocols and
result files were not rewritten. Relevant `.txt` files include model weights,
contracts, token windows, dynamically derived word sidecars and provenance;
their extension is not a deletion criterion.

## Corrections made

- Replaced stale live-status narratives with source interfaces, limits and
  links to dated evidence; separated MTK, capsules, core checkpoints and packs.
- Distinguished default deterministic serving from opt-in GPU/core work and
  the still-open daemon resident lifecycle.
- Corrected web Bearer/auth behavior, mutating query authority and the daemon's
  hard-disabled legacy teacher branch.
- Corrected managed native-library dependencies, CPU/AMD boundaries,
  unavailable endpoints and design-only scheduler/adapter/telemetry features.
- Documented actual scriptlet timeout, schema, speculation, cache, model-swap
  and positional-scaling limitations.
- Replaced stale build/install recipes and marked unresolved legacy harvesting,
  packaging and anti-collapse-probe behavior explicitly.
- Repaired one existing Make documentation gate: its mojibake dash did not
  match the unchanged canonical UTF-8 sentence. Baseline RED was reproduced;
  the same phrase/floor now passes. No runtime algorithm or threshold changed.

## Validation

Fresh builds/regression ran in detached private worktree
`/tmp/cnet-doc-regression-8VVvgw`, using the source checkpoint plus the cleanup
overlay and exact retirements. Later edits only tightened documentation and
recorded results; native source and the tested Make repair were unchanged.
No live model, database or service was started/replaced by these checks.

| Check | Result |
| --- | --- |
| `make verify` | Exit 0; all 28 suites fresh under the run sentinel |
| Managed Llm unit project, Release | 1,147 passed, 36 skipped, 0 failed; skips are not device evidence |
| `cnet_dc_invent`, `cnet_dc_egraph` | PASS; retired-path deny-before-open probes still work |
| Capsule guide example | Input 3 → verified 180; input 6 → nonzero covered-plan refusal |
| Standalone CMake agent-memory target + CTest | 1/1 passed in a new build directory |
| Multimodal prepare test | PASS; retained plan remains a real test input |
| Framing, execution-tier doc, license metadata, claims | PASS |
| Release-integrity authority test | PASS; not the clean-tree release umbrella |
| Inventory/archive/deletion audit | All entries, hashes and replacement paths verified |
| Current-guide local file links | Checked; no missing local targets |
| `git diff --check` | PASS |

An independent migration review confirmed only the 47 approved deletions,
safe regular-file archive members, all 366 protected hashes and the retained
historical amendments. Independent source review found and corrected the
web, receipt, evaluation-coverage, tool-policy, managed API, distillation and
direct-LoRA-forward wording errors. No Claude/Grok call was made.

### Explicit negative result, not a cleanup regression

`make cert_coverage_harvest` returns exit 2 because its existing shell script
calls removed `tools/roe_daily_packs_seed.py`. The failure was reproduced in
the private worktree. The guide marks that command blocked and distinguishes
the separate native pack/router gates. No obsolete Python implementation was
restored, and no test was weakened to conceal the failure.

The packager's old Python evolution assumption, unchecked distillation probe,
cooperative scriptlet boundary, unauthenticated managed sample server and
existing control-plane SQLite advisory are documented code follow-ups.
This cleanup does not claim to remediate them.

### Local log identities

Logs are retained locally under `/tmp/`, not portable release artifacts:

| File | SHA256 |
| --- | --- |
| `cnet-doc-verify-20260906.log` | `da76565b813e618afed828778c282e4c93e06f698a7846cf31c4f410da412d06` |
| `cnet-doc-focused-20260906.log` | `2c181693163ce623de7c842b0fceb24b4e537785e8aab5fa970ea0bc404acf6e` |
| `cnet-doc-managed-unit-20260906.log` | `9190e6c8629dd632808bf2aa83fb6fd32d55cf777ce937ea87f0511f2036017b` |
| `cnet-doc-cmake-20260906.log` | `f845633cdeb0c6f8023645165aded76c2373ca780dd13c7965d869401a0d5a87` |
| `cnet-doc-capsule-example-20260906.log` | `5a56925530a47b214e4d47de6680115729f4783d4021052298cb5c8b93b5c9f2` |
| `cnet-doc-legacy-harvest-20260906.log` | `5c9736ec6fe0943ceee96839cf9c27a7b0894693f3b52f34acbb652cc3956763` |

## Preserved workspace and scope

The main `master` worktree was not changed. In the experimental worktree,
pre-existing `cce.dll` and six dirty decimal weight files remain untouched.
No push, merge, publication or live rollout occurred.

Documentation/migration and independent-review practices influenced the
cleanup: hidden text consumers were retained, exact history was archived
before deletion, and authority claims were checked against executable source.
See [the cleanup contract](../plans/documentation_cleanup_20260906.md).
