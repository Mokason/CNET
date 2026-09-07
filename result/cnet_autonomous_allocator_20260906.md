# Autonomous allocator experiment — September 6, 2026

**Failed held-out gate; candidate inactive. No demonstrated autonomy gain and
no 72-hour unattended run started.** GPU training completed, but the candidate
failed the unchanged outcome-gain, confidence and family non-regression
requirements. No certification floor was lowered and no autoactivation occurred.

This is an offline allocator experiment using existing certified capsules and
an **unsealed generic `teach` acquisition path**, not production admission of
that path. Resource limits on the experimental teacher do not establish the
production worker boundary. Finite random graph instances are not evidence of
broad semantic competence. Hashes establish byte identity, not authentication;
receipt authenticity, source authority and split custody remain trusted-owner
and experiment-author responsibilities.

At the time of this experiment, the C worker pool was not the managed production child
boundary: its monotonic deadline excludes suspend, it has no parent-death
binding, and stderr is inherited without validating its writable-file authority.
Filesystem seals do not revoke an already-open descriptor. These inherited
limits require a trusted bounded offline runner; no unattended GPU-worker
deployment is established. Production `allocator_enabled=true` still refuses.

September 7 follow-up: worker commit `3c408b8` addresses these three lifetime,
deadline and inherited-stderr gaps. See the
[expansion report](cnet_operational_expansion_20260907.md) for CPU verification,
new Linux requirements and outstanding actual-device qualification. This does
not retroactively change the binaries/results below or pass the allocator gate.

## Evidence and chronology

Local artifacts, not a portable archived release bundle:

- Direct diagnostic: `/tmp/cnet-allocator-limitation-5q59np2_/` (`L` below).
- Composition experiment: `/tmp/cnet-allocator-composition-33nf3kop/` (`C` below).
- Training/evaluation executables: `/tmp/cnet-allocator-build-vRYV06/` (`B` below).

`C/freeze.json` fixes the protocol before any outcomes. Acquisition then writes
`C/summary.json`: its literal `checkpoint_trained=false`, `gate_invoked=false`
and confirmation-utilities-pending fields describe that **earlier phase**, not
the final result. Later `training_freeze.json` fixes both training commands,
4,096 epochs, primary device 0 and repeat device 1; it records
`heldout_utilities_inspected=false` and `retuning_on_confirmation=false`.
`candidate_freeze.json` records both checkpoint hashes before the held-out gate.
`primary-gate.json` subsequently records training's candidate failing evaluation.
`failure-analysis.json` is read-only post-gate diagnosis, with `no_refit=true`
and `not_a_second_gate=true`. These phase records were not rewritten.

The confirmation outcomes are now exposed: they cannot be reused for retuning
and then claimed as fresh held-out evidence. Any revised experiment needs a
new prospective freeze and fresh confirmation instances; the floor stays fixed.

## Composition workload and actual training

There were 64 distinct random finite graph instances: 32 development and 32
confirmation, with eight instances of **each** family in **each** split:
direct, shared-prefix, chain-completion and evidence-rejection. The recurring
motifs are not 64 distinct task families. Seeds were 690606311 and 690606313.
Each instance offered four tasks, one job and the same prospective work budget
of 32 rows; all ages were zero. Task subgraphs had disjoint ports; no multi-job
union-synergy gain is claimed. Primitive coverage was 32 uniformly sampled
keys in a 64-key domain. Each episode had 32 fresh demand probes, excluding
observed root/goal/input requests.

The acquisition summary records 648 initial capsules, 240 successful candidate
acquisitions, 16 deliberately corrupt-evidence rejections before `teach`,
89,600 full-domain checks and zero wrong verified answers. These are measured
finite-domain outcomes, not an allocator gain or a production admission result.

Both AMD/HIP GPU devices actually trained 128 development rows for 4,096 epochs.
The recorded measurements in `train-device0.json` and `train-device1.json` are:

| Device | Outer BOOTTIME seconds | Worker elapsed milliseconds | GPU/CPU prediction max absolute error |
| --- | ---: | ---: | ---: |
| 0, frozen primary | 0.33211673400364816 | 329 | 7.4505806e-09 |
| 1, frozen repeat | 0.33270141296088696 | 332 | 7.4505806e-09 |

Both commands exited 0; both worker records report `valid=1`, exit 0, signal 0
and 192 result bytes. Parity compares GPU and CPU predictions of the resulting
cell on the 128 development rows, not an independent CPU training trajectory
or held-out outcome gain. The two serialized checkpoint hashes are identical.
These are observed single-run end-to-end/worker timings, not kernel-only
benchmarks; GPU model names and kernel-only timings are absent from these records.
Both training results explicitly set `approved=false`.

## Frozen primary gate: FAIL

`C/primary-gate.json` reports 32 confirmation episodes, three controls, exit 3,
`passed=false`, `autoactivation=false`, no execution refusal and empty stderr.
Elapsed BOOTTIME was 0.0012353119673207402 seconds. No active learned-cell
comparator was supplied (`active_training_sha256` is empty).

Coverage is the mean newly answered fresh-demand fraction; gains are absolute
fractions, not relative percentage improvements. Control order follows
`CNET_ALLOCATOR_CURSOR`, `CNET_ALLOCATOR_DEMAND_COST`, then
`CNET_ALLOCATOR_COMPLETION` in `include/cnet_core_allocator.h`.

| Policy/control | Coverage | Candidate minus control | Paired 95% lower bound |
| --- | ---: | ---: | ---: |
| Learned candidate | 0.064453125 | — | — |
| Rotating cursor | 0.0556640625 | 0.0087890625 | -0.0110909969 |
| Demand/cost | 0.06640625 | -0.001953125 | -0.0178745509 |
| Completion-first | 0.0615234375 | 0.0029296875 | -0.0143305813 |

The unchanged gate requires gain at least 0.05 **against every control**, a
strictly positive paired 95% lower bound (recorded multiplier 2.040), and no
negative family mean gain. All three gain and confidence requirements fail.
Family gains, in the same control order, also show actual regressions:

| Confirmation family, eight instances each | Cursor | Demand/cost | Completion-first |
| --- | ---: | ---: | ---: |
| Direct | 0.0234375 | 0 | 0 |
| Shared-prefix | 0 | -0.01171875 | -0.01171875 |
| Chain-completion | 0.015625 | 0.015625 | 0.0234375 |
| Evidence-rejection | -0.00390625 | -0.01171875 | 0 |

Post-gate analysis finds an outcome-aware best-single-job oracle mean of
0.0927734375. Its gain ceilings against the same controls are only
0.037109375, 0.0263671875 and 0.03125: even this inadmissible outcome-aware
selector cannot reach the unchanged 0.05 floor on this confirmation workload.
Oracle family means are 0.1953125, 0.0625, 0.0625 and 0.05078125, respectively.
This limits this benchmark's headroom; it is neither candidate gain nor proof
that allocator gain is impossible on other workloads. Development has 53
unique feature vectors, 25 with multiple observed utilities, involving 90 rows.
The fixed feature representation therefore also aliases materially different
outcomes. Neither observation authorizes weakening the gate.

## Earlier direct-task diagnostic, not a second gain gate

`L/summary.json` reports 32 distinct episodes but **one repeated direct-task
feature-aliasing design only**: family counts `[32,0,0,0]`, 16 development and
16 confirmation episodes, 98 tasks, 25,088 all-key checks and zero wrong verified
answers. Distinct random tables/probes do not supply the missing three families.
The deterministic diagnostic cell was untrained; no gate was invoked or passed.
All four policies had coverage 0.02197265625; the three paired gains and paired
95% lower bounds were all zero. In 27 episodes tasks had differing utilities,
despite identical online features forcing tied policy choices. The outcome-aware
best-single-job mean was 0.038899739583333336.

The diagnostic's own withheld reasons are zero gain from identical-feature
choices, absent required families, only 16 confirmation episodes and an
untrained model. It is a counterexample to feature expressiveness on this
workload, not an impossibility result or evidence of independent-family gain.

## Recorded commands and byte identities

These are the saved training/evaluation arguments, not a new run. For brevity,
`B` and `C` below expand to the exact absolute directories named above; `E` is
the confirmation SHA256 in the table. The original argv arrays are preserved
in `training_freeze.json`, `train-device{0,1}.json` and `primary-gate.json`.

```text
B/allocator train B/allocator_worker 0 C/development.tsv C comp0.cell E
B/allocator train B/allocator_worker 1 C/development.tsv C comp1.cell E
B/allocator evaluate C comp0.cell C/development.tsv C/confirmation.tsv
```

Actual source/data/checkpoint/executable file hashes were checked against the
recorded freezes where present. `L` and `C` contain different frozen versions
of `allocator_campaign.py`; they must not be conflated.

| Artifact | SHA256 |
| --- | --- |
| `C/protocol.json` | `5ed426f92df592d54ec8608486660a8846e4eaf7867a5376866cfd561159ccfa` |
| `C/allocator_composition_campaign.py` (producer source) | `e368c50ab16c48144e54b2da62f81e7ce2e14331f4669743c05d0d9cc99510d1` |
| `C/allocator_campaign.py` (support source) | `24497abb1c0f87d87a72c6c6899cac5382a73986a403d2e86cdefebeb938f635` |
| `C/bin/cnet_capsule_core` (compiler) | `b53af2a4fed849b8d41b040a6835badaffa151dc78fba12d2d14791e2504cf56` |
| `C/bin/cnet_capsule_tool` (oracle) | `b14804262a9c29598cecf175a27accf72873f609a5a89655ead7dff23163008a` |
| `C/bin/libcnet_capsule_core.so` (runtime) | `7f55c07d2f7651f569a751ce9379fae7029077038527630874af7befbd8e97be` |
| `B/allocator` (trainer/evaluator) | `7ca32abbf44550d0fc76f206c513260f0bf2ab9f6b1d64479aace5ad5ee1c892` |
| `B/allocator_worker` | `7a1fa4861956ab6c2d16f5bd48dd54ec43f7c486c61ac80185a029dd633cc476` |
| `C/development.tsv` (training) | `93228ce48bad78f0fe1a7d6391d25ac18e33e75123afced29759a9c28638fb73` |
| `C/confirmation.tsv` (evaluation; `E`) | `9972f2e3abab348107337a12eb3c3dcf0a40e6e58b2f38c8516b06a0e676f72c` |
| `C/comp0.cell` and `C/comp1.cell` | `89835974474a43abf31a8cd052dafabdef7feaeef405598628f23ba7819c5796` |
| `C/training_freeze.json` | `836f5e95ac7d77bf5aa8b95a129fc237fea103e213ca34f30f9426665862c6c0` |
| `C/candidate_freeze.json` | `3a658edeafc6118e2ff967b9e31a70072d3c06741ec5a8e554f73d86154198e8` |
| `C/primary-gate.json` | `6272575648a7d2884c7c915c87ec23071a19eb551c93f8fae579e50cebe7e845` |
| `C/failure-analysis.json` | `7aee33ec21329922d62e8701e253ecb0a47f45669dd4bb8af893a5d429614ef8` |
| `L/allocator_campaign.py` (diagnostic source) | `a287f560935e4b87858729231a77523ebc722e81c7b174c6b514e25cdcfe214d` |
| `L/development.tsv` | `379568b76ea964418f3edf0de372d5c811768d433082821fc635c826cd5ca621` |
| `L/confirmation.tsv` | `80c87ffacf388da5e359a312ccd4dbc04766d59f7be8c6c1a0b2074924e607c1` |
| `L/diagnostic.cell` | `28316e5997f851ba969883105d37f1f8c97a7cb707544d4ae4f0174f6134ce2e` |
| `L/summary.json` | `0c550e8a27f47bf5beff3546fc35c066929a2e5c3d938be1fdd4e1edc5b03532` |

The original recording above did not rerun, refit, activate or modify its
artifacts. Production autonomy gain remains **WITHHELD**.

## Later build integration and read-only repeat

Final review found the new allocator sources lacked Make targets. An actual
`make -n ... allocator-test` RED reported `No rule to make target`; its output,
but not an explicit exit-code field, was retained in the tool receipt.
The [allocator contract](../experiments/offline_controller/allocator_contract.md)
now gives reproducible private CPU build/test commands and separately opt-in
AMD compilation/execution. CPU targets never invoke HIP, GPU workers or serving
library builds. Dependency dry-runs and header-triggered rebuilds were checked;
the new AMD recipes were inspected by dry-run only, not executed.

A fresh build in `/tmp/cnet-allocator-final-review-PEQZ18` compiled the CLI and
diagnostic chooser and passed all four CPU test executables. A separate
`sanitize/` build passed the same four with
`-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer` (plus the normal
strict C11 warning/include flags). These are policy, parser, gate algebra and
canonical-checkpoint tests, not GPU fitting or measured benefit. The original
build outputs were tool receipts (sessions 78803 and 53667); no saved build log
is claimed. An author-separated boundary review found no new blocker for the
inactive experiment and retained the worker-lifetime/descriptor caveats above.

Main subsequently repeated the already-built tests into
`/tmp/cnet-allocator-cpu-final-20260907.log` and
`/tmp/cnet-allocator-sanitize-final-20260907.log`, both exit 0. For the latter
repeat, `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1` were explicitly set. Neither repeat reported a
sanitizer error on its executed paths; this does not cover the GPU worker or
establish absence of vulnerabilities.

The newly built CPU CLI also re-read the original frozen checkpoint, training
rows and confirmation rows. `/tmp/cnet-allocator-gate-replay-final-20260907.log`
records the expected exit **3**, identical coverage/gain/confidence values and
`autoactivation=false`. This is a reproducibility check of an exposed failed
evaluation, **not fresh confirmation**, retuning, a second gate or new training.
No original experimental artifact was overwritten.

| Later artifact | SHA256 |
| --- | --- |
| CPU repeat log | `e10ddc0df8698a6e7accb896a9c1c64419e88755a33fcda76cfabfb1cf68a2a8` |
| Sanitized repeat log | `d6f97d59c0cc9a4845b6bc2d086782a2b0bf0e733594c933ff72b060654f574a` |
| Failed-gate replay log | `8c559300a14167475c491ede01516f3f6f33b927826a97e22e2a0427cca77665` |
| Fresh CPU allocator CLI | `d6c9032e963ead582711fc1044aa8c1b675e32fbf4995b69de67bf4582157d1f` |

The subsequent [sequence feasibility pilot](cnet_allocator_sequence_pilot_20260907.md)
is development-only and uses actual complete native trajectories. It measures
deterministic planning value, but no residual headroom for a learned gain over
its exact control; it does not reinterpret these v1 outcomes or relax this gate.
