# Dual-AMD GPU product sequence: measured result

Date: 2026-09-06. Branch: `experiment/offline-controller-20260906`.
Scope: bounded offline experiment and opt-in core APIs, not a live deployment.
The implementation sequence is complete. Final frozen-state capability
certification is pending at this evidence checkpoint. Security release clearance
is **WITHHELD** because the separate managed control plane has an existing High
dependency advisory described below.

## Working path

`AMD FP32 training → immutable snapshot → independent evaluation → direct BTN
weight conversion → existing capsule certification/export/import → shadow and
complete history replay → explicitly activated, request-pinned generation`.

Two isolated workers can run on the two Radeon AI PRO R9700 cards simultaneously.
They cannot activate the host. Cancellation, abnormal exit, partial output,
timeout, capacity refusal and restart are tested. The ordinary worker binary and
the separate fault-injection binary both pass their applicable tests; their
independently fitted snapshots are byte-identical.

The new shared cell has 41 FP32 parameters (164 bytes), learns an explicitly
specified local Boolean propagation rule, and is applied repeatedly to a typed
graph. This is a bounded task-specific mechanism, not language understanding or
autonomous acquisition of specialist knowledge. Training uses an explicit
Boolean specification followed by independently BFS-labelled correction rows,
never CNET Tier-A answers. The warm-up supplies the recurrence; it is not
discovered by the experiment. Native fixture/weight files are experimental
artifacts; the product checkpoint is a separate canonical, versioned 404-byte
core-model record, not a second capsule format.

The worker-trained cell transfers by direct weight copy to the existing BTN and
capsule format. It passes all eight certified Boolean rows at minimum margin
0.489318189 (unchanged floor 0.05), prediction-copy error 3.1082705e-8, all eight
growth obligations, and out-of-domain refusal. Capsule payload: 861 bytes,
excluding manifest and runtime memory. Exhaustive certification of eight rows is
not a held-out population accuracy measurement.

The opt-in host retains up to four immutable model/registry generations and 64
request leases. Stage/check/activate/rollback are explicit; old requests keep
their old generation. Promotion checks the actual neural serving path against
all sealed-label joins, not just a small shadow sample. Coverage, contract and
audit checks remain mandatory at every execution hop. The experimental graph
admits at most 62 capsules plus two endpoint nodes; the unchanged deterministic
runtime still supports 4096 capsules and remains the default.

## Benchmarks

All timings are on shared hardware, without GPU reservation or clock changes.
Training numbers use the earlier fixed network, 128 updates, batch 128, FP32,
three rotated-order repetitions and the same unchanged 1e-4 parity gate. The
41-parameter selector is a different workload; these speedups must not be
attributed to its tiny training task.

| Fresh benchmark | GPU0 | GPU1 |
| --- | ---: | ---: |
| Resident rocBLAS median | 21.742 ms | 33.982 ms |
| Four-thread OpenBLAS median | 57.693 ms | 57.424 ms |
| Ratio of CPU/GPU medians | 2.65× | 1.69× |
| Individual GPU runs | 21.742 / 21.017 / 21.931 ms | 23.621 / 70.433 / 33.982 ms |

GPU1's variability is retained; earlier approximately 21.4 ms results are not
substituted for the fresh sample. Maximum sustained rocBLAS weight error is
6.85453415e-7. FP16/BF16 sampled matrix shapes failed the unchanged forward
tolerance and are **not admitted**. An unstable repeated-single-batch FP32
diagnostic also remains recorded; arbitrary long trajectories are not claimed
bitwise reproducible.

The clean-build actual-serving benchmark performs 9000 requests per repetition
(8000 verified answers and 1000 OOD refusals), three alternating-order repeats:
median-of-p50 latency 2.624 us deterministic versus 9.949 us opt-in neural, with
one certified capsule. Neural p99 ranges 10.370–11.501 us. This explains why
deterministic serving stays the default; GPU training acceleration does not
justify moving tiny live requests onto a GPU.

Six frozen models (three seeds, ideal/on-policy variants) each achieve 640/640
completion and 640/640 negative-case raw-score abstention in each of two
1280-case graph suites at sizes 8/16/32/64. That is 2560 generated cases and
15,360 case-model pairs, not 15,360 distinct graphs. One-iteration controls solve
only 25/640 positives. The added, separately frozen stress suite contains mixed
productive/dead regions and a legal dead branch in every positive; no weights
were retuned. Original positives were uniformly productive, chain topologies
repeat under relabelling, and both training variants tie. No on-policy benefit
or broad task transfer is established. BFS is about 140–195× faster on these
small graphs. Maximum family-size Brier is 7e-8.

Cell device tensor allocation is 385,192 bytes, additional to HIP/runtime/code
allocations. Measured worker child maximum RSS is 220,228 KiB (about 215 MiB), not
the simultaneous workers' aggregate RSS. The earlier larger rocBLAS owner has
about 42.56 MiB tracked steady device allocation, 74.56 MiB transient peak, and
about 296 MiB cold process RSS. Model bytes, tensor scratch and runtime overhead
are deliberately reported separately.

## Regressions and security checks

Raw evidence: `cnet_gpu_product_evidence_20260906/final/`; graph/model evidence:
`cnet_gpu_product_evidence_20260906/selector/`. The execution ledger and
`plans/cnet_shared_selector_20260906.md` detail the fixed protocols and earlier
resident results.

- Fresh `make verify`: all 28 source suites passed.
- Knowledge capsule, accumulation, coverage/abstention, composition, learning
  health, growth, budgets, selected audit, history coverage, demand security and
  AMD math gates passed.
- `make capsule_capacity`: 64-to-66 inventory boundary, 6338 replay obligations,
  duplicate/corruption refusal passed; deterministic capacity was not reduced.
- End-to-end product test passed both with the existing output directory and
  from a new empty build directory. The latter caught and fixed a missing
  output-directory prerequisite, with RED evidence retained.
- Existing CPU, GPU, encoding, resident, rocBLAS and stream experimental
  regressions passed on both discrete devices. Cell independent finite-difference
  gradient error: 1.32886747e-7 on each GPU.
- Candidate, selector and host ASan/UBSan tests passed. Existing capsule
  sanitizer passed 94 checks with leak detection. Its initially stale compile
  flags were fixed; both failed-build and successful-run evidence are retained.
- Compiling the new C components with GCC `-fanalyzer` produced no diagnostics.
  The empty `static_analysis_compiled.log` records that run; the earlier
  syntax-only invocation is not counted as static analysis coverage.
- Negative tests cover corrupt/incompatible/NaN checkpoints, every byte flip,
  recomputed invalid headers, symlinks, FIFOs, permissions, truncated/trailing
  data, stale evidence, incomplete neural coverage and denied worker mutations.
  A scoped high-confidence secret-pattern scan found no matches; this was not
  an exhaustive repository-history scan.
- `capability_cert` initially refused because concurrent work changed its bound
  untracked state. Its 34 runner unit tests passed, but that invocation is not
  certification. A frozen-state rerun is required; the fingerprint/floors are
  unchanged.

Independent adversarial review and RED-first tests materially changed the
implementation: they closed a neural-promotion coverage bypass, filesystem
ioctl and queued-signal sandbox bypasses, a parent EOF/exit race, and saturated
GPU-loss reporting. The security and review skills also required distinguishing
an audited boundary from a claim of general isolation. No optional Claude/Grok
CLI was invoked; the exact-command offers remained unanswered.

Landlock ABI >=3 protects filesystem writes before HIP creates threads, with
only GPU devices, `/dev/null` and a job-owned scratch directory writable. COMGR
requires that scratch directory; denying it caused a reproducible HIP startup
crash, resolved without granting writes to global `/tmp`. Network and process
restrictions apply after trusted HIP warm-up and cover its threads. Results
require an exact private IPC record, EOF, clean exit and CPU/GPU parity. This is
not a hostile-native-code service: filesystem reads are allowed, HIP startup and
the driver remain trusted, and total RSS/disk/driver consumption is not a hard
quota. No physical GPU reset was injected or other user's process interrupted.

### Open High-severity dependency finding

The main `dotnet/CNET.slnx` dependency audit reports no vulnerable packages, but
does **not** include `CnetControlPlane`. Auditing that project separately reports
`Microsoft.Data.Sqlite 8.0.11 → SQLitePCLRaw.lib.e_sqlite3 2.1.6`, affected by
**CVE-2025-6965 / GHSA-2m69-gcr7-jv3q**. SQLite before 3.50.2 can corrupt memory
when aggregate terms exceed the available column count. The advisory lists no
patched version in that old native-package family; merely bumping to 2.1.11 is
not a fix. See the [reviewed advisory](https://github.com/advisories/GHSA-2m69-gcr7-jv3q)
and [SQLite fix](https://www.sqlite.org/src/info/5508b56fd24016c13981ec280ecdd833007c9d8dd595edb295b984c2b487b5c8).

Reachability review found parameterized row inputs and no untrusted raw-SQL
route. However, both ingestion and activation accept a supplied `--db` path;
ingestion's column-name check does not reject arbitrary triggers/views and
activation opens without schema validation, including dry-run. A substituted
attacker-controlled database can therefore reach native schema/SQL preparation.
This is a conditional exposure assessment, not a demonstrated CVE exploit.
Deployed database ownership/permissions were not audited, so trusted-file
containment is not established by this run.

Concrete remediation sequence, separate from the new C/HIP path: migrate to a
maintained compatible native bundle, or `Microsoft.Data.Sqlite.Core` with an
explicit provider; verify the loaded native SQLite is >=3.50.2 or has the
documented fix backported; test JSON functions, ingestion transactions and
activation/resume; rerun all control-plane tests and transitive dependency
audit. Microsoft documents the [custom bundle/provider mechanism](https://learn.microsoft.com/en-us/dotnet/standard/data/sqlite/custom-versions).
Until then, use only protected trusted database files. No dependency upgrade,
audit suppression, OS package upgrade or live configuration change was made.
This check is not a complete OS/ROCm-driver vulnerability audit.

## Reproduce and deployment boundary

From this worktree, with ROCm/HIP for `gfx1201`, a C compiler and Linux Landlock:

```sh
make -C experiments/offline_controller product-test
make -C experiments/offline_controller test gpu-test investigate-test resident-test rocblas-test stream-test
make verify
make knowledge_capsule_sanitize capsule_capacity
# No concurrent tracked/untracked source or evidence writes during this gate:
make capability_cert > /tmp/cnet-capability-cert.log 2>&1
```

The product test uses only new private `/tmp` artifacts and never activates a
live registry. Explicit stage/check/activate APIs are available to an owner;
there is no unattended promotion daemon. Learning new semantic domains,
larger-scale neural inventory admission, durable live rollout/crash recovery,
resource isolation beyond the stated worker contract, and the SQLite migration
remain separate scopes. No master merge/push or live service change is included.
