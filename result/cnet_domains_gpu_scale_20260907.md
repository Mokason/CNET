# Domain adapters, AMD qualification and capsule scale — 2026-09-07

Scope: owner-requested tasks 2, 3 and 4, from worktree
`CNET-worktrees/autonomous-learning-20260906`, baseline `2eab134`.
No existing live service, GPU job, deployment policy or ledger was changed;
the dirty main checkout was untouched. Tests used fresh private deployments.
Task 1 continuous/72-hour acceptance was not included. No push is implied.

## 2. Bounded symbolic acquisition — PASS

Added canonical `CNET_LOCAL_SYMBOLS_V1` evidence to the existing schema-2 capsule
asset, not a new package. Exact ASCII keys select literal labels through the
source-bound certified unit. Numeric `data` queries keep their numeric behavior;
`symbol` queries terminate on refusal without teacher fallthrough. The managed
`learning lookup` command normalizes known-token demand under an immutable
keys-only vocabulary hash. Unknown tokens never borrow another ordinal. Labels
can be refreshed through normal acquisition; vocabulary changes require a new
policy/deployment. The `.05` certification margin is unchanged.

Independent staged evaluation checks all 256 encoded inputs AND actual native
token/label observations, including one absent-token probe. Numeric identity
alone, wrong/constant text decoders and answered unknown tokens cannot pass.
Live verification and probation use the same bounded text checks. Probation
short-circuits on a known failure and persists rollback duty before another
exchange can fail or be cancelled. Ordinary verification is observation-only.

### Real-source workload

The pinned, licensed Unicode 17 excerpt independently supplies these two tables:

| Dataset | Source bytes | Exact known keys | Property |
| --- | ---: | ---: | --- |
| `ascii_category` | 2,076 | 95 | General_Category |
| `ascii_bidi` | 2,020 | 95 | Bidi_Class |

Keys are official names for U+0020..U+007E with source-name spaces replaced by
underscores, then ASCII-sorted; `HYPHEN-MINUS` retains its official hyphen.
This is literal finite lookup, not unseen-text interpretation or a bidi algorithm.
The managed integration test derives expected rows independently from the pinned
raw semicolon fields, without calling the extractor or the production decoder.

A new private deployment imported both sources, observed uncovered requests,
ran for its originally provisioned 30 seconds with one-second ticks and three
successful probation sweeps per capsule, accepted both, and settled with
`budget_complete`, two jobs and no pending intent/outstanding job. The final
same-generation sweeps measured, per dataset:

- 95 correct encoded answers, 161 correct encoded abstentions, zero missing/wrong;
- 95 correct literal text answers, one correct absent-token refusal, zero missing/wrong.

The absent-token checks are **not exhaustive text-space OOD coverage**. Source
row fidelity is not proof of unseen-domain performance. Separate lifecycle
tests cover label-only refresh, exact quotes/backslashes/spaces, stale refusal,
vocabulary-change rejection, coexistence, and rollback after changed evidence.

Pins from the full regression's real workload:

| Identity | SHA256 |
| --- | --- |
| Raw Unicode excerpt | `75dfecc13fe9b1202e3f7c787e4e7f2c848c97c8b092dd75b4f6a2b99990cdc4` |
| Keys-only vocabulary | `ba3fe8b2440a6065c04e714136c1ff742e7e7ab772902ff4558fe3b2699b6984` |
| Category source | `f967c633b453c9e8a4d38ddf55484f16bce97d5fac6e60275e9adbe1484e848a` |
| Bidi source | `c3c00de586e292da4ad197624b3aaef598a8d170c27283b251a99ed9d4523759` |
| Managed installation manifest | `dac525ccb998f07355f03a6edc961b7287ef3e6f7b64b78d3a4cafe2c051f424` |
| Native installation manifest | `fbfdd4f73f21573de454e7cb979c082b3ec3da6019784a63447561446a78fa21` |
| Policy | `83306c7a4c7e639a57ca78429cf15af227a573a748a1e3d0fa726f4868c1494d` |
| Final active inventory, revision 3 | `d58ed170596a892c267988bddeaf9fd796373c66ba1bee841709484f1ed97e6d` |

### Regressions and review

- Full managed suite: **884 passed, 0 failed, 0 skipped**, repeated at 40 and 38 seconds.
- `make learning_symbol`: native table reader/sandbox and numeric producer/verifier
  gates; existing daemon acquisition/refresh/rollback; 8 new native symbolic
  tests; 5 extractor tests; 42 selected managed tests, all passed.
- `knowledge_capsule`: 94 checks passed.
- `coverage_abstain`: 55 checks passed; named existing held-out fixture 4/4.
- `knowledge_accumulation_bench`: existing 32-unit mechanism gate passed; it is
  distinct from the new independent-function scale measurement below.
- `knowledge_composition_bench`: three-member composition passed with coverage
  guarded at every hop and root/intermediate refusals.
- Astra adversarial review found and resolved deferred probation failure
  persistence under caller cancellation. The old two cases failed before the
  fix; four numeric/text × missing-control/cancelled-control cases now pass.
  Wrong/constant decoder, policy/source compatibility, socket identity, literal
  injection boundaries and byte/resource bounds are covered. This is scoped
  hardening, not a claim of absence of all vulnerabilities.

Retained full receipt:
`/tmp/cnet-domains-final-20260907/managed-full.trx`, SHA256
`423c2c7a1e1469438491e7e508c15c0d11da213fb0b9383738034f84c3ecc42f`.
Probation RED `/tmp/cnet-symbol-workload-20260907/probation-red.trx`, SHA256
`b4b2ef4e96ea5d4d6d1e66b93d597347aebd0a211522a05c870f934bec246558`;
expanded GREEN `probation-symbol-green.trx` in the same directory, SHA256
`003bd0d1e3c0eb5f41ad3f386d3d54be7e5f2339ba37d27be5d3dbdbc5b81b78`.
Temporary receipts are local evidence, not durable deployment artifacts.
The post-code-commit repeat is
`/tmp/cnet-domains-final-20260907/managed-postcommits.trx`, SHA256
`77c8591467e2d7a9530815b09324df126a8bf80b7405aae3034fb9b1b55303ee`.
Existing Linux-platform analyzer and native build warnings remain; tests passing
does not mean the whole repository is warning-clean.

## 3. AMD worker qualification — PASS; useful allocator gain WITHHELD

Current private HIP worker builds and **22 CPU boundary cases passed**. Allocator
CPU/parser/checkpoint/gate tests and all three AMD worker executable builds also
passed. Build/CPU results are not actual-device qualification.

Both discrete GPUs continued hosting live work. After the owner explicitly
approved concurrent execution, the prepared production-worker and allocator
numerical gates passed on devices 0 and 1, taking 0.67 and 0.43 seconds respectively.
The production fixture passed frozen transfer and cancellation/restart; allocator
CPU/GPU maximum absolute error was 2.98023224e-08 on each device, below the unchanged
1e-6 threshold. All completed workers returned valid 192-byte receipts and exited 0.
Six monitored services retained their PIDs/start times/restart counts, four HTTP
health endpoints returned 200, and post-test VRAM matched the pre-test snapshots.
No service was stopped and no device was reset or reserved. These are finite
numerical/lifecycle fixtures, not sustained throughput or service-latency tests.
Physical suspend/resume and forced in-flight GPU fault qualification remain
WITHHELD. See the [actual-device results and exact receipts](cnet_gpu_qualification_20260907.md).

The existing learned allocator's frozen failed confirmation remains unchanged:
coverage 0.064453125 vs demand/cost 0.06640625, gain −0.001953125, paired-95% lower
bound −0.0178745509. Required gain remains 0.05; no refitting to exposed holdout
results. The existing one-CPU-second exact control proved all 16 development
optima, mean coverage 0.423828125, maximum CPU 0.822701866 s. Even bundle-greedy
leaves only 0.0283203125 oracle headroom on these receipts, less than the gate.

The scoped generators contain seeded/shuffled demand, not independently collected
chronological future requests. There is no eligible residual chronology here for
a defensible new future-demand fit. Next work requires that independent history,
a preregistered development workload with headroom over all fair controls, and
fresh whole-episode confirmation under unchanged floors. GPU throughput does not
substitute for useful controller gain. `allocator_enabled=true` still refuses.

Build/CPU qualification receipt directory:
`/tmp/cnet-gpu-qualification-20260907-nSt4GL`; `QUALIFICATION.md` SHA256
`f6740f0bd5afc4ef54b50078eef07e144e67eae5da8b5d9c0775e12b43261280`;
`build-and-cpu-gates.log` SHA256
`cd376da57409c9c619486929052ce8d89c8fb84c2574690e6276a8b823a05367`.
That earlier report preserves the pre-authorization state and prepared commands;
the linked actual-device report records their subsequent execution. The stale
worker-limits section in `allocator_contract.md` now describes the already
implemented BOOTTIME/thread-pidfd/stdio guards accurately; no stronger device
qualification is inferred from that documentation correction.

## 4. Distinct resident capsule scaling — PASS

Final reviewed-runner qualification passed **12/12 fresh-process measurements**,
three each at 32, 256, 1,024 and 4,096 distinct synthetic capsules. No correctness
floor, inventory limit or core guard budget was changed. See the
[full measurement contract and tables](../plans/capsule_scale_20260907.md) and
[retained final raw JSONL](cnet_capsule_distinct_scale_20260907.jsonl).
The reproducible entry points are `make capsule_distinct_scale_contract` and
`make capsule_distinct_scale_bench`; old replica-based scale targets are preserved.
These are small five-row synthetic capsules, not acquired knowledge or a memory
estimate for arbitrary rich capsules.

| Final N | Serialized N bytes | Median incumbent N−1 RSS, MiB | Median N−1 + N overlap RSS, MiB | Covered p99 range, µs |
| ---: | ---: | ---: | ---: | ---: |
| 32 | 74,671 | 9.26 | 9.79 | 4.07–4.40 |
| 256 | 597,305 | 11.40 | 13.91 | 6.27–7.64 |
| 1,024 | 2,389,193 | 18.81 | 28.67 | 13.09–16.22 |
| 4,096 | 9,556,823 | 48.91 | 88.24 | 33.41–34.91 |

RSS is total process residency, including library mappings and bookkeeping;
it is not serialized payload size or a universal per-capsule constant. At 4,096,
median incremental overlap above baseline is 80.375 MiB. Median incumbent load
was 1,645.48 ms, candidate load/preserve-stage 1,811.10 ms, activation 0.000771 ms
and rollback 0.000531 ms. Those last two timings are in-memory pointer/generation
operations, not the cost of loading a set, durable publication or socket queries.
The timing populations are warmed serial native asks; no cold-filesystem-cache,
parallel serving or GPU performance claim is made.

Each 4,096-capsule replicate checked 20,480 covered answers, 1,028,096 exhaustive
uint8 OOD refusals and 4,096 sampled neighboring cross-port refusals, with zero
wrong verified answers. Across all 12 measurements: 81,120 correct answers,
4,072,224 OOD refusals and 16,224 cross-port refusals. Every replicate checked
distinct identities, preserve-stage, activation, old/new pinned views, rollback
and refusal to close a host while leases remain pinned. Interface isolation
sampling is not every possible cross-unit pair.

Final raw receipt SHA256:
`aa568dbb6f828fdeb30dd17234b1a51858c2138dcf259c39cf844f634c64111a`.
Runner SHA256:
`0ffa2821973755d4355e852c6f6c4f616a59e65916f3916ac40477ecf82072e1`;
native library SHA256:
`2856e370280c56d4f3ca474cc7896398681c764109ecaa493516f3dbce7135dd`.
All 10 recorded input/library pins were unchanged after qualification. Final
runner contracts: 10 tests passed, with independent focused accounting/lifetime
review. Performance came from the final successful run, not selected rows from
the earlier failed attempt.

An attempted final qualification refused after six accepted measurements. Its
[raw failure receipt](cnet_capsule_distinct_scale_failed_20260907.jsonl), SHA256
`5904db39dc9c3a74e1cb6275bbadc5f718f40cb075e87f34f0fbcff6a64268dd`, is retained.
The next child printed PASS, but the parent could not confirm resource accounting;
that child is not counted as accepted evidence. The old generic diagnostic does
not establish the exact branch. Follow-up tests independently reproduced missing
fields on a completed child; the fix checks exact child exit for both unavailable
read variants without ignoring failures on live children. Exit status and disk
limits remain mandatory. Earlier review also closed ordinary SIGTERM cleanup
races. Hard-kill/abnormal parent death cleanup is not claimed, and the benchmark's
300-second monotonic deadline excludes suspend.
