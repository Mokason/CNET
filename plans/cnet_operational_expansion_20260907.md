# Operational expansion after private table deployment

Status: IN PROGRESS. Baseline `3457f5c`. Existing services, policy, ledger and
dirty main checkout remain untouched. The owner requested proceeding with the
five outstanding product areas, not relaxing any failed gate.

## Sequence and acceptance

1. **Independent live verification** (managed command, verifier, focused tests).
   Check all 256 numeric inputs against owner-authorized source bytes, including
   every abstention. Bind results to unchanged source and durable active native
   revision before/after; bound the entire sweep with suspend-inclusive time.
   No demand creation, training, native mutation, or implied run acceptance.
   Verify RED first, adversarial timing/source/native changes, and real daemon
   learned/unlearned/stale cases. No dependencies.
2. **Dataset onboarding** (source command/helper and focused tests).
   Provide explicit owner publication of approved canonical evidence, with
   dataset authority, byte/hash/path limits and no overwrite or self-labeling.
   Verify malformed, unauthorized, duplicate and resource refusal before writes,
   then the real acquire/activate/verify path. Depends on 1 for live validation.
   Choosing a useful real corpus remains an owner input; do not invent approval
   or call synthetic fixtures production knowledge.
3. **GPU worker hardening** (existing experimental worker boundary and tests).
   Add suspend-inclusive deadlines, parent/spawning-thread lifetime protection
   and removal of writable inherited output authority. RED tests precede fixes;
   CPU tests and opt-in actual AMD jobs validate the bounded worker contract.
   No allocator activation or claim of arbitrary-code GPU isolation. Independent
   implementation may proceed alongside 1; root reviews before commits.
4. **Acceptance preparation** (bounded receipt collector/checker and tests).
   Use independent repeated live probes and original run identity. Missing,
   truncated, reordered, under-duration or discontinuous evidence must refuse.
   A short campaign is not 72-hour qualification. Depends on 1 and 3; a full
   product acceptance run additionally retains the useful-controller gate.
5. **Useful controller and rollout**.
   Obtain real chronological workload evidence with residual headroom against
   strong deterministic controls before a new GPU fit. Keep the original
   minimum gain, confidence, family and safety gates unchanged; no test-set
   retuning. Production integration/promotion requires independent confirmation.
   Deploy only a reviewed immutable private installation with explicit workload
   and duration; upgrading the unrelated existing live service needs a precise
   migration decision. Do not reset a terminal ledger to renew its budget.

## Trust boundary and review

The owner authorizes source contents; a hash proves identity, not truth.
External bytes and native observations are untrusted inputs. Reuse the existing
CNB/CNU1 packaging, strict managed parser, private filesystem access, native
control protocol and typed ASK client. No new teacher, network acquisition,
credentials, database migration or policy-schema change is assumed.

Each slice has a RED/GREEN checkpoint, review across correctness/security/API
boundaries, documentation and a separate local commit. Use Astra-only fresh
review; the owner already declined external-model review. Missing skill
references do not override repository gates. Decisions stay in `plans/`.

## Task checklist

- [x] Live verification command and focused + native integration tests
- [x] Owner evidence onboarding command and end-to-end tests
- [x] GPU worker hardening and CPU regression
- [ ] Real-device regression of the changed GPU worker boundary
- [ ] Bounded acceptance evidence preparation and refusal tests
- [x] Combined regression, audit, documentation and reviewed commits for delivered slices
- [ ] Owner-selected real workload and broader domain adapter certification
- [ ] Useful controller confirmation and guarded integration
- [ ] Actual 72-hour acceptance (WITHHELD until elapsed time and criteria pass)
- [ ] Deliberate production rollout

## Live verification checkpoint

Initial real-entry test failed with `LEARNING_LIVE_VERIFY_RED` and the old CLI's
usage refusal. The implementation then passed its real native daemon check.
Astra author-separated review found that per-exchange deadlines did not bound
pending I/O after suspension against the whole-sweep budget. Two additional
`LEARNING_LIVE_DEADLINE_RED` cases reproduced that gap before the fix. The
verifier now polls the global boot clock while waiting, cancels pending clients
and awaits their cleanup. Second review found no further required issue; its
cancellation/invalid-initial-clock test suggestions were added.

Focused command/live/ASK/control/reference regression: **167 passed, 0 failed,
0 skipped**, six seconds. The 25 live-verification cases include one real daemon
end-to-end test; synthetic clocks are fault tests, not elapsed acceptance.
Equal native STATUS fences bind the revision/state tuple, not continuous daemon
process identity. Source/runtime digests prove identity, not source truth.

## Import and worker checkpoints

Import's initial real-entry test failed with `LEARNING_IMPORT_RED` before its
implementation. The quota fixture initially used an unsupported 1 MiB policy;
that fixture was corrected to the existing 16 MiB minimum, without changing
production limits. Import now passes 16 focused cases, including exact-fit and
one-byte overflow, and an additional author-separated actual-entry durability
test. A fixed test-only fsync fault after rename causes refusal, retained exact
bytes, released locks and refused overwrite retries. No production fault hooks
were added. The 262143/262144-entry import boundary is source-reviewed but not
physically exercised; no large-inventory qualification is claimed.

Worker commit `3c408b8` passed 22 CPU boundary cases, the existing CPU/sandbox/
allocator regression and AMD compilation. Focused ASan/UBSan runs also passed,
subject to intentional SIGKILL/_exit and discarded child-stderr limits. Actual
GPU execution was not attempted because unrelated live services occupied both
devices. The old services, device allocations and private deployment are intact.

Final delivered-slice managed regression: **670 passed, 0 failed, 0 skipped**,
13 seconds; native table daemon, capsule/coverage/accumulation/composition gates
also passed. Production and test NuGet audits reported no known vulnerabilities.
See `result/cnet_operational_expansion_20260907.md` for commands and exclusions.
The continuous acceptance collector/checker, real approved workload, useful
controller confirmation, actual 72-hour run and production rollout are still
outstanding; the new live probe is only a prerequisite for that work.
