# Discord brick capacity repair — 2026-09-08

## Outcome

Source repair: `06eede7`. The live Linux daemon and both factory/mint consumers
now use the bounded 256-entry legacy LUT bank. Existing live inventory remains
24 files, byte-for-byte unchanged. This is storage headroom, not new certified
capabilities or a change to any admission floor.

The owner requested publication before diagnosis. The 24 completed commits
through `890860e` were first fast-forwarded onto `origin/master`. Eight local
and fourteen remote ancestor refs were then removed with expected-tip guards.
Unmerged refs, checked-out branches, all worktrees and unrelated dirty/private
primary-checkout data were retained.

## Diagnosis and changes

A read-only query of the existing owner DM found the 2026-09-08 02:16:33 UTC
capacity-failure diagnostic. The local witness independently confirmed 24 LUTs,
factory maximum 24, serving maximum 24, projected count 25, and no mint attempt.
No Discord message was sent and no raw DM history or credentials were published.

Both limits are now 256. Reload and the global compose loader replace a complete
validated snapshot or retain the old bank and counters. Overflow, malformed
tables and duplicate tags cannot expose a partially loaded startup bank.

The common publisher validates canonical addressable tags, bounded metadata and
the existing 16-integer domain. Cooperating publishers lock the directory inode;
a new file consumes a free slot while replacing an existing entry does not.
Private temporary files are atomically renamed after flushing. Failure is loud;
a directory fsync failure can be reported after publication and is explicitly
marked as such. A failed configured publish is propagated before moving the
pending student's ownership into a parked brick.

Evolve no longer treats a failed inventory load as zero entries or emits its OK
marker for invalid before/after inventory. This does not make a whole evolve tick
an all-or-nothing transaction across its legacy subsystems.

## Verification

RED witnesses were captured before their corresponding fixes: the 25th load and
256-entry boundary, failed global reload, 257th publication, invalid metadata and
values, whitespace-tag aliases, lost factory persistence errors, false evolve
success on malformed/overflow input, and missing native startup capacity evidence.

| Gate | Measured result |
|---|---|
| `make core_brick_capacity` | 256 entries; 4,096 table/key fixture checks; no raw certification; overflow, duplicate/malformed reload retention, publisher limits, competing writers and ownership refusal pass |
| `make core_serve_authority` | Invalid table/input and raw-certificate refusal pass; publisher refuses an invalid prior bank |
| `make core_serve_untagged` | 8 addressed/unaddressed selection checks pass |
| `make core_bus_untagged` | 6 bus selection checks pass |
| `make core_bus_tensor_refusal` | 4 explicit tensor/fallback refusal checks pass |
| `make core_evolve_brick_capacity` | 3 tests pass, including valid 25/256 counts and rejected malformed/257 inputs |
| `make cnetd_brick_capacity` | Isolated native 25-table load and UNIX-socket PING pass |
| ASan + UBSan + leak detection | Expanded native capacity/publication/ownership fixture passes |
| Live-compatible binaries | Same daemon and evolve process tests pass against the staged release |
| Real-bank rehearsal | Copy of actual 24 LUTs loads; native mint of `blk.16.attn_qkv.weight` creates `q1_27b16` only in the private copy; new process loads all 25 and answers PING |
| Capture regression | 66 existing capture tests passed before the initial publication; capture code unchanged by this fix |

The 4,096 checks are 256 named fixtures times 16 inputs, with repeated fixture
behaviours. They do not establish 256 distinct learned capabilities. Inline
`CnetServeBank` measures 42,528 bytes and `CnetCoreBus` 384,104 bytes; dynamic
student/registry allocations are not included.

One existing untagged test expected raw LUT proof despite unchanged authority
code rejecting that claim. The same two failures were reproduced against the
pre-fix source before correcting the test to require calculation without
certification. No certification assertion was relaxed.

## Live deployment and rollback

Private release and originals:
`/home/marble/.local/share/cnet-brick-repair-20260908-HyaMGl/{release,before}`.

Only six runtime files were backported into the older dirty primary checkout:
`include/cnet_core_bus.h`, `include/cnet_core_serve.h`,
`src/serve/cnet_core_bus.c`, `src/serve/cnet_core_serve.c`,
`tools/cnet_core_evolve.c` and the startup diagnostic in `tools/cnetd.c`.
All other local edits, deployment configuration and private learning state remain
unpublished and untouched.

Primary-compatible daemon, mint and evolve binaries were rebuilt into the private
release. The existing capsule library was copied and frozen during linking:

```text
libcnet_capsule_core.so 529e139956a72d2795a3dc4f39d63f4aa6e44946f57e40c931ba6796aed7982e
cnetd                  d7b4913024ea0ed9bbba5e3dcf6ee22b4f6cb806e1a2dcc8e2f68ef508bee737
cnet_pq2_brick_mint     7c5c05350a430acb979f6748a332ab1b36ae5b230b5432b86b66a1dbcfb8083e
cnet_core_evolve       db69aee4ed754301650f8f32ec85b118cfac4d4309ca7f055990010ba4eba2a7
```

The dedicated `cnetd.service.d/50-brick-capacity.conf` override selects this daemon
and factory. Primary mint/evolve executables were atomically replaced, retaining
their originals. The existing timer will also rebuild the patched primary sources.
Only cnetd was explicitly restarted; capsule environment pins, model services,
private learner, capture scope and provider policy were not changed.

Observed live PID 1876721, zero automatic restarts, correct release executable
and frozen capsule library mapped. Startup reports
`{"event":"brick_bank_loaded","count":24,"capacity":256,"certified":0}`.
Live socket PING passes; local model health ports 8080, 8081, 8083 and 8092
return HTTP 200. All 24 LUT hashes still match the pre-rollout inventory.
The Discord bridge, private learner and mouth processes retained their
immediate pre-rollout PIDs. The bridge already had four automatic restarts before
this rollout; this repair does not claim to diagnose that separate history.

For rollback, first validate inventory: the old executables cannot load more than
24 entries. Never delete newly acquired knowledge just to roll back. With the
original 24-file inventory intact, move only the dedicated override into the
private backup, restore the saved mint/evolve executables atomically, reload the
user service manager and restart cnetd. The original primary cnetd executable and
capsule library were not overwritten. Reverse only the six backported source
changes if they have not subsequently been edited; do not reset the dirty tree.

## Limits and separate observations

This is a Linux/POSIX publication implementation. Windows publication explicitly
fails with ENOSYS pending equivalent guarantees; Windows runtime support is
WITHHELD. Directory locks cover cooperating publishers, not arbitrary external
editors. Reload remains a single-owner API.

No live 25th LUT was installed, no automated improver/provider was invoked, and no
new certified capability is claimed. The 27B rehearsal LUT stays in the private
test copy. Full product/GPU/capability benchmarks and a full vulnerability audit
were not rerun. Broader capture dataset/headroom/72-hour gates remain WITHHELD.

The feature branch's shared-library build emitted existing macro-redefinition and
table-reader compiler warnings; they are not declared fixed by this scoped repair.
The live deployment retained the original library.

The unrelated pre-existing `cnet-capture-alert.service` start-limit failure remains
recorded; monitor/watchdog health and desktop alert delivery are separate gates.
No blanket all-services-healthy claim is made.

## Deleted branch receipt

Every tip below is reachable from published master. Recover a local name with
`git branch <name> <saved-tip>`; do not overwrite an existing name.

| Scope | Removed ref | Saved tip |
|---|---|---|
| local | `feat/unified-self-improvement` | `684a50ea75079bbd1a23e330a346d70447991bd4` |
| local | `feature/cnet-7b-competition` | `a811b126db65250f16266750d2417bb5fdf73455` |
| local | `feature/vision-object-detection-benchmark` | `bd73047032c509346f8d7e191398756874d96558` |
| local | `feature/vision-portable-specialist` | `7a89c4eff26be5ae1b36b391f4b209ca9f250a12` |
| local | `fix/build-warning-debt` | `0b5006df6ad5039c04b50aa9ce11d9092b4edd13` |
| local | `fix/build-warning-debt-clean` | `00731b5fed1c5ae5ebf9659c4803ffa53cc49d94` |
| local | `fix/release-integrity-fail-closed` | `6d4931057f447004adc152b8ab1a872ee3f356a0` |
| local | `priority/integration` | `0b5006df6ad5039c04b50aa9ce11d9092b4edd13` |
| origin | `feat/aicimo-harness-lane` | `40d001579922cc7b8c31b29608f40667366491c4` |
| origin | `feat/c-speak-wrap` | `3297853ce75fd36df32cdac3fd84162b0abc4f49` |
| origin | `feat/capsule-loop` | `0fcd8aca34e39420a724a415aa67e277d5c8c94f` |
| origin | `feat/capsule-swap-law` | `192396cdabcfd67acfd0123b4f47cb1a97ed1df8` |
| origin | `feat/chat-lookup-hop` | `6de0d3c22004508d443d2860814e1865c162c151` |
| origin | `feat/crc-intent-recover` | `87c9673621c4b4d3033f10e5613531f3bf7c5283` |
| origin | `feat/dc-egraph-sleep` | `0d8639eac9e08df75d677e36d6549be69d26ab60` |
| origin | `feat/dc-sleep-traces` | `fdb3987c09872eabeb558259eb124dda99bffea8` |
| origin | `feat/library-swap-law` | `627310597016048326d57ab4c2bd201b5b298f0e` |
| origin | `feat/weight-conversion-door` | `f3f35aa49c7c80224628bc323a668985876adcb6` |
| origin | `feat/weight-gguf-labeler` | `354371f1cd34aaf33d6746dd6414b33dcd216906` |
| origin | `feature/cnet-7b-competition` | `fdb3987c09872eabeb558259eb124dda99bffea8` |
| origin | `feature/vision-portable-specialist` | `7a89c4eff26be5ae1b36b391f4b209ca9f250a12` |
| origin | `worktree-cnet-brain-split-core-eb6376` | `acce7e9ac3bfb0a9104e0c24c22f88b2c3177a63` |
