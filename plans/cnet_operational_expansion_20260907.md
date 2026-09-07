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
- [ ] Owner evidence onboarding and end-to-end tests
- [ ] GPU worker hardening, CPU and real-device regression
- [ ] Bounded acceptance evidence preparation and refusal tests
- [ ] Combined regression, audit, documentation and reviewed commits
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
