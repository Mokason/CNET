# Demand-driven capsule growth — September 6, 2026

## Delivered scope

Implemented an opt-in product loop over the existing daemon and curriculum:

```
uncovered typed request → bounded intent spool → operator-approved integer tool
  → existing evidence job → certification + mandatory historical replay
  → immutable capsule → verified answer on the next request
```

Serving never calls the teacher or treats its own answers as evidence. The
independent native tool supports unsigned integer multiplication and XOR.
Acquisition covers at most 32 policy-approved inputs per job. Evaluation uses
that same finite block; no unseen-input generalization is established.

Four prerequisite repairs accompany this path:

- Authority verification builds the ROE front-door executable without invoking
  its production-relative seeder.
- Conversion grammar accepts case/whitespace variants while preserving exact
  interface tags and terminal clarification/refusal behavior.
- Value-aware breadth-first search preserves shared covered suffixes and
  positive identity cycles, through the existing strict guarded executor.
- Every CLI publication checks exact guarded joins of sealed labels in both
  old and proposed inventories. Optional evaluation cannot omit this check.
  Guard rows lacking sealed labels refuse; no capsule format was added.

Search is capped at eight hops, 1,024 states and 65,536 charged operations,
including searches made during replay. Replay, contract consistency and
coverage-label binding have separate 2,000,000-operation budgets and
cooperative two-second deadlines. Loading/certification is outside these
deadlines; these are not hard real-time guarantees.

## Verification

- Genuine RED-to-green regressions cover the four prerequisites, missing demand
  capture, independent tool behavior, internal work limits and FIFO-lock refusal.
  Early new test-fixture errors (missing daemon route fixture, incorrect shell
  return status, near-miss `a`/`b` interface names) were corrected; the corrected
  socket regression was rerun against the old daemon and failed at missing
  demand capture before passing against the new build.
- Socket demand growth: baseline unverified miss, then **32/32** correct verified
  answers; a second block preserves earlier answers; a second XOR domain and
  process restart pass. Unsupported domains and unsafe policies refuse.
- Spool security: 256-request cap, normalized deduplication, eight concurrent
  producers, malformed inputs, unsafe permissions, symlinks and lock contention.
- Acquisition security: overflow, malformed/symlink evidence, invalid/duplicate
  policy, terminal unsupported-request capacity recovery, fair scanning,
  deterministic job retry and 4,096-job saturation.
- AddressSanitizer/UndefinedBehaviorSanitizer with leak detection pass for the
  new spool implementation and internal core-budget fixtures. This is not a
  sanitizer claim for the entire linked runtime.
- Full isolated `make verify`: **28/28 suites**, fresh-log binding passed,
  including the expanded authority suite.
- `make capability_cert`: **6/6 certified**, run
  `da1475d2-7d18-44bd-82a3-bf2607bb8dd5`. Graded classification **0.861** and
  tool-call adapter **0.775**, unchanged. Other 1.000 scores describe only their
  existing named held-out fixtures, not broad competence.
- Portable capsule: **94 checks**; coverage/abstention: **55 checks**;
  accumulation: **32/32 isolation**, **8/8 replay**, **96 OOD refusals**;
  composition: three members with coverage enforced at every hop; learning
  health gate passed. The serial verification command exited **0**.
- `git diff --check` passed. ROE dependency isolation passed, and the copied
  route/catalog fixture hashes still match the source fixtures after the audit.

The isolated audit directory is `/tmp/cnet-demand-audit-NUkmiK`, copied from the
dirty workspace with its Git metadata. Source files were stable throughout the
audit. Its logs are `/tmp/cnet-demand-full-verify.log`,
`/tmp/cnet-demand-capability.log` and `/tmp/cnet-demand-knowledge.log`.
Focused workspace log: `/tmp/cnet-growth-final-focused.log`.
The capability report is in that audit directory's `logs/capability_cert.json`,
bound to worktree SHA-256
`b8bb174be64a4389279658669ca02706ee4bdb5bf472fd5ce6e42a59534ade41`.
It is not a claim that the dirty workspace equals the historical HEAD commit;
this final report and checklist were written after the copied-source audit.

Key source SHA-256 identities in the audited tree:

| File | SHA-256 |
|---|---|
| `src/serve/cnet_capsule_core.c` | `335e702af0c8e17c45406094120f8373cd978a37cbc20321d48ebbda9b4d94bd` |
| `src/serve/cnet_capsule_demand.c` | `cca4b2729a88421baacdcfa3b1e2e1985c61bfd8fd2c0c6fe6d4d2494f669e16` |
| `tools/cnet_capsule_tool.c` | `3ac9ea7b16a15961cf5c4f5422913a3cab2ec932d4a212a94b8dd5d225ff8533` |
| `scripts/cnet_capsule_acquire_tick.sh` | `e828bb94b5a82b23907b5ebcdf60e59c4e7da4ca2822d1e876a6faf479eb880a` |

## Review and operational state

Independent design/code reviews were reconciled. Review required terminal
unsupported-demand handling and producer-side queue capacity enforcement;
both were fixed and independently retested. Per-search admission budgets now
match serving, and contract comparisons are bounded. A suspected signed-zero
join issue was disproved by an actual seal/load fixture, not patched blindly.
No external cross-model CLI review was invoked without user approval.

The implementation/interface/security/documentation skills shaped the ordered
RED-to-green slices, owner-local policy boundary, existing-format reuse and
explicitly measured limits. No certification floor was lowered.

No services were restarted and no new live acquisition policy was enabled in
this slice. Built binaries and opt-in scheduler integration are ready locally;
the running daemon remains PID 2224263. Read-only deployed health passed with
one mined unit, one coverage record, zero unguarded/unreadable mined units and
the residual teacher reachable. Existing autonomous learning was not stopped.
Unrelated dirty-worktree edits were preserved; no commit was made.

## Activation and remaining scope

See [usage and limits](../docs/CAPSULE_CORE.md#opt-in-demand-acquisition) and
[example tool policy](../config/capsule_tools.example.tsv). Set owner-private
demand/queue/capsule paths and a mode-600 approved policy, then use the existing
autoteach schedule or run `scripts/cnet_capsule_acquire_tick.sh` manually.
Enabling capture in an existing daemon requires its normal configured restart.

Remaining gates are unknown-domain teacher discovery, broader language intent
fidelity, held-out capability transfer, larger-inventory latency/cost and
full power-loss recovery across legacy writers. These remain WITHHELD; the
finite acquisition proof does not establish them.
