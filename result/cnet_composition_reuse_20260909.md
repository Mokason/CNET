# Verified composition reuse — September 9, 2026

Continuation of `fad5013` on `feature/verified-task-core-20260908`, implemented
through `6c89b6a` with additional fault/coverage tests in `d2e1054`. Work stayed in
the isolated development worktree. No push, deployment, schema migration, live
capture, source approval, GPU fitting or frozen-soak mutation was performed.

## Delivered behavior

`cnet_capsule_core reuse ABS_SOURCE ABS_DESTINATION REQUEST` selects only the
capsules used by a successful verified request. Existing capsule bytes, guards
and optional source assets are retained. A fresh staged core must reproduce
selected identities and the same verified value/hop/unit receipt before
no-clobber publication. Whole-source validation includes unselected artifacts.
Ordinary asks never export or train. No second package format was introduced.

The resulting content-addressed directory is a reusable capsule inventory, not
a fixed recipe or newly certified composed domain. Different covered inputs
are replanned through the same per-hop verifier. See the
[operator contract](../docs/COMPOSITION_REUSE.md) and
[decision](../plans/cnet_composition_reuse_20260909.md).

## Actual-native useful workflow

Independent arithmetic produces hours → minutes labels for hours 0–17 and
minutes → seconds labels at multiples of 60 corresponding to hours 0–16.
The existing finite compiler and unchanged certification floor build two
ordinary capsules. A third, unrelated doubling capability remains in the source.
Physical capsule directory names deliberately differ from sealed unit names.

- Exporting 2 hours selects exactly two capsules and verifies 7200 seconds.
- All 17 covered values, hours 0–16, succeed from a freshly reopened subset.
- Hour 17 refuses at the intermediate boundary; 18 and 31 refuse at the first.
- The unrelated capability remains usable in the source but is absent from the
  exported inventory. Every selected artifact matches the source byte-for-byte.
- Removing the arithmetic source inventory does not prevent subsequent replay.
- Identical export retries resolve the same digest; a changed existing snapshot
  refuses and is not overwritten.

These are synthetic, independently labelled mechanism tests, not genuine usage,
an independently frozen paraphrase population or learned-policy improvement.
No successful CNET answer is reused as a training label.

A separate schema-2 source-fact fixture proves byte-identical asset retention.
The copy answers while its explicit source pin is current, then refuses after
the source changes or disappears. After restoring the source and demonstrating
successful export again, removing only the asset causes refusal. Arithmetic
offline success is not a claim that source-dependent facts work without sources.

## Verification

| Check | Result |
| --- | --- |
| New actual-native reuse integration suite | 8 tests passed |
| Direct production validator/API fault fixture | 12 checks passed |
| Snapshot suite, including selected publication | 20 checks passed |
| Full managed control-plane regression | 961 passed, zero failed/skipped; 1 minute 24 seconds |
| Python capture/bridge regression | 148 passed, zero failed/skipped |
| Full-linked-C ASan/UBSan gate | Six executables passed, including reuse validator; leak detection enabled |
| Strict native syntax/warning check | Modified core, snapshot, reuse and CLI compiled with `-Werror` |
| New reuse module executable-line coverage | 52/52 |
| Changed executable lines in snapshot/core | 42/42 and 19/21 respectively |

The two uncovered added core lines report directory-name allocation failure.
Whole-file line coverage was 174/206 for snapshots and 495/637 for the core;
those broader files include existing lifecycle/selector paths outside this
coverage fixture. Counts combine gcov line hits across the production callback
and its directly compiled fault fixture. These are **line** measurements, not
branch completeness, mutation testing, fuzzing or absence of vulnerabilities.
The coverage build instruments the three changed native modules and links other
symbols from the normal runtime; the sanitizer gate instruments the full linked
runtime separately. Existing unrelated build/platform warnings remain.

Affected native regression also passed: resident lifecycle, store/fault/recovery,
source evidence and reserved interfaces, daemon lifecycle/control, core growth,
composed acquisition/refusal, value-dependent search, historical coverage,
finite compilation, publication interfaces, intent parsing and publish recovery.
Its private 20-cycle daemon fixture recorded 120 correct answers and 20 OOD
refusals. That short fixture is not the independently running 72-hour soak.

## Review and security scope

Fresh-context Astra review produced three actionable corrections:

1. Validate the whole source before the core's directory scan and while copying,
   so a selection cannot hide malformed unselected or dot entries.
2. Isolate artifact-symlink tests from unrelated top-level-file refusal.
3. Restore valid source freshness before testing missing-asset rejection.

Direct fault tests alter expected identities, values, hop counts, selected unit
receipts, selected counts and coverage requests. Every mismatch leaves no new
published or pending directory. Existing invalid digest contents are retained.
Traversal, unsafe permissions, links, FIFOs and invalid publisher metadata refuse.
The final bounded review found no remaining concrete issue in the corrections.
No other model provider was called.

A new fixture initially used a group-writable directory and was correctly
rejected before its intended callback. The permission error was diagnosed and
fixed in the fixture; no production boundary was relaxed. This preliminary RED
was not counted as evidence that callback fault injection had been exercised.

The source must remain owner-stable; neither the old nor new boundary promises
a transaction against hostile same-UID writers. No production dependency was
added, and no fresh advisory database scan was claimed in this slice. Source
truth, hostile OS/owner compromise and power-cut certification remain outside
these checks. Post-publication durability or output errors can retain a complete
artifact while reporting failure; identical retries never overwrite it.

## ECC and Graft trial

ECC's TDD/verification workflow required executed RED before production code:
`315c101` recorded both missing-feature markers. Focused GREEN preceded
`17142c6` (validated subsets) and `6c89b6a` (reusable export), followed by expanded
fault tests, full regressions and coverage. The package-manager detector again
reported npm as a default without a repository package manifest; actual work
used the existing C/.NET/Python runners, not unrelated npm tests.

MCP graph lookup reported this worktree unindexed. Graft supplied exact source
spans and caller context instead. Its refreshed local wiring graph contains
1,998 indexed files, 24,112 nodes, 24,300 edges and 1,970 map cards; 10 files were
parsed and 1,988 replayed from cache. The wiring freshness check passed. The
optional meaning/deep tier remains unbuilt; no model pass or external provider
was used. Estimated token savings are not presented as measurements of progress.

## Reproduction and evidence

```sh
make capsule_reuse capsule_reuse_coverage capsule_product_sanitize
make capsule_product_closure capsule_control capsule_core_growth \
  capsule_composed_acquire capsule_composed_refusal capsule_value_search \
  capsule_history capsule_history_coverage capsule_finite_compile \
  capsule_publication_interfaces capsule_intent capsule_publish_recovery
dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj \
  --artifacts-path .artifacts/task-verified --no-restore --nologo
```

Capture tests use the same private independently compiled evidence fixture and
existing Python environment documented in the
[previous handoff](cnet_capture_task_inbox_20260909.md). No package install occurs.
Local, uncommitted evidence:

- `/tmp/cnet-reuse-snapshot-red-20260909.log`
- `/tmp/cnet-reuse-core-green-20260909.log`
- `/tmp/cnet-reuse-native-regression-20260909.log`
- `/tmp/cnet-reuse-managed-20260909.log`
- `/tmp/cnet-reuse-capture-regression-20260909.log`
- `/tmp/cnet-product-sanitize-6rd3xK/`
- `/tmp/cnet-reuse-coverage-F8BDIh/`
- `/tmp/cnet-reuse-coverage-review-20260909.log`
- `/tmp/cnet-reuse-graft-build-20260909.log`
- `/tmp/cnet-reuse-graft-check-20260909.log`

Temporary integration fixtures remove only their own test files. No user data
or live installation was deleted. Durable local fixture/coverage evidence stays
under the named private temporary roots.

## Remaining sequence

Next: independently frozen paraphrase/OOD evaluation of the existing bounded
natural-language proposal route. Richer multi-field/document workflows and
dependency-aware source replacement remain separate extensions. Then pursue
targeted AMD task-policy candidates only with independently verified chronological
experience, measurable useful headroom and the unchanged promotion gates.

Real origin review, proposed-source UX, acquisition-cost measurement, controlled
rollout and the original real-duration/useful-gain gates remain open. This slice
does not change the disabled allocator or invent eligible genuine episodes.
