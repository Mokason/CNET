# CNET implementation and evidence — September 5, 2026

## Outcome

Implemented and deployed a runnable, externally teachable typed capsule core,
integrated with the actual daemon and the existing learning scheduler. The
approved base migration is complete and deployed learner health passes. This
is a bounded working foundation, not completion of the requested LLM-like
breadth or autonomous cross-domain evidence acquisition.

The executable sequence is:

```
typed intent → external tool labels → native BTN fit/compile → contract verification
  → existing capsule export/import → atomic publication → guarded local execution
  → immutable coverage expansion → reuse after restart
```

`make capsule_frontdoor` executes that sequence against a temporary daemon over
its Unix socket. Its first capability converts six certified minute values to
seconds. A second converts seconds to metered credits. An intermediate value
outside the second capsule's coverage refuses. A separately taught version adds
that coverage while retaining the previous answers. Corrupt manifests refuse.

See [usage and limits](../docs/CAPSULE_CORE.md) and the
[ordered implementation plan](../plans/cnet_authority_and_growth_20260905.md).

## Repairs implemented

| Area | Concrete change |
|---|---|
| Fallback authority | Resolve and execute the actual admitted alternative; refuse ledger-only, incompatible/mutated and uncovered targets. |
| Raw LUT authority | Require complete finite integer tables and complete integer requests; raw files never assert proof/certification. |
| Learning evidence | Shared bounded JSON reader; positive external-source allowlist; own/local, duplicate, malformed and unverified rows cannot vote. Independent review is mandatory for non-gold proposals. |
| Promotion failure | Removed the tick's direct `certified 1` writer. Failed front-door admission cannot advance promotion state. |
| Capsule serving | Reuse CNB, CNU1 and existing manifests, replay certification, enforce per-hop coverage, reject contradictory contracts and support request-local covered alternatives. |
| Publication | New teaching CLI verifies import before atomic no-clobber directory publication; immutable versions coexist. |
| Artifact integrity | Capsule import rejects embedded NULs and non-whitespace data after the manifest checksum. |
| Test evidence | Failed/empty managed test runs cannot emit PASS; corrected stale evaluator source paths, archive link order and the consolidation fixture's report binding. |
| Build integrity | CCE header dependency files and compiler/flag invalidation; modular capability rules; default verification includes serving/provenance/core regressions. No certification floor or Makefile ceiling lowered. |
| Deployment inspection | Health reads the actual running lane's effective environment and checks executable generation instead of guessing a base path. |

The documentation and interface/security skills shaped the explicit trust
boundary, immutable publication, negative tests and recorded limits. The code
review also found empty-array `qsort` undefined behavior and missing cleanup in
the tick; both were fixed and checked under sanitizers.

## Measured evidence

| Check | Result |
|---|---|
| `make verify` | All 28 suites passed with log freshness binding |
| `make capability_cert` | 6/6; run `b519969b-9843-4601-9142-834992ca4b0b` |
| Classification | 0.861 on the existing graded fixture |
| Tool-call adapter | 0.775 on the existing graded fixture |
| Capsule core regression | 33 checks, zero failures |
| Actual socket growth | Baseline 0/6 local answers → 6/6 correct local answers; asserted chain, restart, expansion and OOD/corruption cases passed |
| Portable capsule | 94 checks passed |
| Coverage/abstention | 55 checks passed |
| Existing composition benchmark | 74 checks passed; three members, guard at every hop |
| LUT/tick sanitizer runs | AddressSanitizer + UndefinedBehaviorSanitizer + leak checks passed |
| Whitespace/diff check | `git diff --check` passed |

The current machine-readable capability report is `logs/capability_cert.json`.
The older `logs/capability_cert/report.json` is a July artifact, not this run.
Other current evidence is in `logs/authority.log`, `logs/capsule_frontdoor.log`,
`logs/capsule_core_growth.log`, `logs/knowledge_capsule.log`,
`logs/coverage_abstain.log` and `logs/knowledge_composition_bench.log`.

The managed test runtime was missing; .NET 8.0.30 was installed alongside the
existing runtime using Microsoft's installer with non-versioned host files
preserved. No runtime was removed. [Official installer documentation](https://learn.microsoft.com/en-us/dotnet/core/tools/dotnet-install-script).

## Approved deployment and follow-through

The user's subsequent approval authorized the migration and restarts. The
original base and the excluded `hyb_struct_bonsai` were preserved separately
under `artifacts/deployments/certified-core-20260905/`. The new `active.cnb`
retains 1,095 units, verified byte-for-byte against the source's sealed payloads.
It initially contains no mined units; its explicit empty coverage store grants
no invented authority. The original source was not overwritten. Live health
now reports zero unguarded mined units, coverage enabled, and teacher reachable.

`config/certified-core-deployment.env` and user-systemd
`zzz-certified-core.conf` drop-ins select the active deployment. The daemon,
shared MCP service and learner were restarted. Related timers were restored.
An actual scheduled maintenance cycle completed and restored the learner.
Legacy mining now seals recorded teacher rows and saves their guard before
checkpointing; configured external writers pause/resume the same-base learner
under per-base locks to avoid stale in-memory checkpoint overwrites.

The additional implemented sequence is:

1. Constrained language proposal (`convert N INPUT_TAG to OUTPUT_TAG`) → typed
   resolution → guarded plan. Recognized ambiguity remains unverified.
2. Existing autoteach timer → bounded queue of trusted external-evidence jobs →
   private snapshot and SHA-256 → candidate training/compilation.
3. Full installed-inventory compatibility check and before/after evaluation →
   serialized no-clobber publication → idempotent retry and atomic receipt file.
   Failed jobs do not block later jobs indefinitely; scan and attempt budgets
   bound each tick. No own-answer log ingestion or teacher API call is added.
4. New immutable capsule versions can expand coverage while old answers remain
   available. A missing capsule value still refuses at every composition hop.

The first larger gradient fit failed certification on 4 of 32 supplied rows;
the failure was not admitted and no floor was changed. A bounded native BTN
finite-domain compiler now provides reliable acquisition when small gradient
fits do not converge (and for more than 16 rows). It encodes supplied mappings,
then passes the same certification/coverage machinery. Receipts name the method
honestly. The 256-row boundary regression verifies every supplied mapping and
refuses the first out-of-domain input. This is not unseen-input generalization.

Two production jobs were admitted from independently computed arithmetic labels:

| Measured stage | Before | After | Wrong certified |
|---|---:|---:|---:|
| 0–31 minutes → seconds, 32 cases | 0/32 | 32/32 | 0 |
| Add seconds → 24-fps frames; direct + composed suite | 32/64 | 64/64 | 0 |
| Separate deployed-socket check after restart | — | 64/64 | 0 |

The deployed socket also passed three negative/clarification cases. It returns
verified `4320` for `convert 3 minutes to frames_24fps`, and refuses second
value `1` for the frames capsule. No teacher calls occurred in this test path;
that does not measure the cost of producing future external evidence.

Further fixes include per-direction tag/signature conflict refusal before
publication, evidence-snapshot race protection, fair scheduling, and an explicit
terminal capsule reply flag: notes and legacy fallbacks can no longer replace
a capsule refusal. ROE publication now atomically replaces its authoritative
JSON catalog, preserves in-memory state on failed saves, serializes front-door
promotions, and makes identical retries idempotent. Tests cover concurrent
promotions, conflicting identities, escaped strings and failed saves. Final
independent review found a long escaped-answer reload bound bug; a RED regression
reproduced it and the bound now covers worst-case encoded fields.

## Remaining limits / claims WITHHELD

- This capsule adapter recognizes constrained forms, not arbitrary language.
  Misses do not automatically acquire correct external evidence or create jobs.
  The scheduler learns only from supplied trusted evidence, up to 256 rows/job.
- Registry capacity is 256 immediate capsule directories with at most eight
  linear hops; inventory reload/certification happens per request. Large-scale
  latency, throughput, cost and broad cross-domain coverage are unmeasured.
- Finite-domain acquisition/compilation proves the supplied mappings and exact
  refusal boundary, not transfer to unseen arithmetic or language competence.
- Atomic catalog/directory visibility is tested. Full power-loss durability of
  all parent directory metadata, transactional ROE derivative SKILL bundles,
  and coordination with every arbitrary direct CNB writer are not proven.
- External provenance labels are a local trust boundary, not authentication or
  automatic proof that an operator supplied true labels.

No commit was created. Pre-existing user changes were preserved. See the
[runbook](../docs/CAPSULE_CORE.md) for queue format, live domains, snapshot
hashes and recovery precautions.
