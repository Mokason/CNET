# Source-evidence closure — bounded module and producer

Date: 2026-09-06. Worktree: `product-closure-20260906`; source module introduced
after distillation commit `49ff8e41b7778a800924cef4536afe68ac965e98`.
Other closure slices were edited/committed concurrently; this report makes no
claim that an aggregate dirty worktree has passed final product certification.

## Contract and decisions

Five named local CNET header-constant facts, independently checked literal
source lines, existing typed BTN certification and existing schema-2 capsule
assets. No teacher, network, arbitrary source execution, second package format,
changed certification floor, live database or service, or automatic activation.
The public opaque API binds exact contract/coverage labels to source SHA256,
extractor version, actual grep receipts and canonical text decoder. Unknown
schema, unsupported intent, stale source and decoder conflict fail closed.

The parent reviewed the API/design before implementation. One review corrected
an overly strict source policy: a group-writable checkout is read-only untrusted
data, not output/control authority. Source reads now allow owner-owned writable
checkouts while preserving nofollow, stable bounded reads and full hashes;
outputs/control remain strictly private. This preserves freshness against the
actual checkout instead of forcing a disconnected private copy. There is no
permission-changing step or hidden opt-out flag.

Final producer review found that output leaf names could exceed the immutable
snapshot loader's grammar/96-byte limit. Four name subcases actually failed
with `SOURCE_COMPONENT_RED` before the fix. The producer now refuses these
before acquisition, and pins the opened output directory for export/reimport.
The pinned path needs `/proc/self/fd/N/.`: without the final directory component,
the existing exporter's nofollow parent-sync correctly returned
`manifest_sync_failed`. The final path passed all exporter tests.
`_GNU_SOURCE` definitions are guarded for shared Make `-D_GNU_SOURCE -Werror`.

The API/interface, security/hardening, incremental implementation, debugging,
review, git workflow and documentation skills shaped the bounded interface,
negative tests and evidence record. Doubt review was routed to the parent;
cross-model review was skipped in this delegated non-interactive context.
The referenced skill definition-of-done file is unavailable in the installed
catalog; repository and product-plan gates are the explicit completion bounds.

## Actual RED and GREEN

- Before production implementation, the new C test loaded the current real
  shared core and failed `SOURCE_EVIDENCE_RED source evidence runtime API exists
  before any success claim`; the symbols did not exist. Receipt:
  `/tmp/cnet-source-evidence-red-20260906.log`.
- Before changing read permissions, the added checkout-like mode test actually
  failed: `SOURCE_EVIDENCE_RED owner-owned group-writable checkout remains usable
  as untrusted read-only data` (38 checks, one failure).
- The producer integration test first failed four tests because the native
  producer was not yet installed in `bin`. This was a supplemental integration
  RED after the producer source had been authored, not the original behavioral
  RED; it is not presented as pre-implementation evidence.
- Before adding test-only publication fault hooks, the pre-export and two
  post-export sync fault cases actually failed (three subcases), with
  `SOURCE_PUBLICATION_RED` / `SOURCE_PUBLICATION_UNCERTAIN_RED` assertions.
  Their GREEN run proves removal before export and explicit exit 3 with complete
  retained files after export. Fault selection is absent from production builds.
- Focused C module: `SOURCE_EVIDENCE_PASS checks=53 failures=0`.
- Real-checkout producer integration: 6/6 tests, zero skips.
- ASan/UBSan with leak detection: the same 53 checks and 6/6 tests, no findings.
  The module, SHA helper, producer and C harness were instrumented; the
  producer's pre-existing linked core library was not instrumented in this
  focused run. Full core sanitizer coverage is a separate integration gate.

The checks cover five independently hardcoded canonical labels and named
intents; actual grep output acquisition and executable/source digests; exact
schema, port order, contract and non-generalized coverage binding; malformed,
truncated, path-mutated and receipt-mutated assets; changed/deleted/root-renamed,
symlink and multiply linked sources; duplicate/missing/expression/oversize
definitions; instruction-like and OOD refusal; decoder conflicts under the same
output interface; full capsule export/reimport/recertification; private output,
collision retention, pre/post-export sync faults and no overwrite. Source-text semantics beyond literal
definitions and unseen capability generalization were not tested or claimed.

## Exact real-checkout artifact

The separately retained private artifact is
`/tmp/cnet-source-real-evidence-FbA94f/facts`, produced from the actual worktree
at HEAD `83ba11b56c17b8620a59b8e7138af0c8b914a77a` with concurrent unrelated edits.
Only the two source-header byte identities below determine these facts:

| Item | SHA256 |
| --- | --- |
| `include/cnet_capsule.h` | `7fc1daccc3600a3b70e745d417c21a5c3105030579f88865a4ccedcde88bffbf` |
| `include/cnet_json_internal.h` | `fe949fca22e3412a92e36e64ed6d8088c397a4f9bec1b413f1cdc42da182a436` |
| actual `/usr/bin/grep` | `20a35a4c18f2fe9e6305c1cbf866b9c0fcc3f957132ae66028c99ec353bb0a80` |
| `frontend.cvfa` | `2b204afd055b643e486f508e48a721b6b68382546512a45d95ca460f837e2bc5` |
| `unit.cnb` | `6ea82dc7717a4c1711f8a5a17acde50741f9a7caedcbefdef6436371dcce129e` |

Actual producer receipt: `SOURCE_CAPSULE_PASS unit=source_facts facts=5
source=verified_tool receipt=literal_line_check ... margin=0.500000
pending_activation=1`. Manifest: existing `CNET_CAPSULE 2`, CNB version 5,
five exemplars, sampled scope, coverage 5×3→3, behavior digest
`16734786351452967163`. This is finite-domain compilation from independently
checked literal evidence, not model-learned generalization.

## Reproduction and bounds

Focused standalone module build used GCC C11, `-Wall -Wextra -Werror -O2`,
`-D_POSIX_C_SOURCE=200809L -Iinclude`, compiling
`src/serve/cnet_capsule_evidence.c` and the existing
`src/cce/cce_campaign_provenance.c` as a private shared library. The C harness
loads that library through `dlopen`; the producer links the existing core and
the new module. Sanitizer runs add `-g -fsanitize=address,undefined
-fno-omit-frame-pointer`, with non-PIE executables, and
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1`.
Tests accept explicit private binary/library paths; Python uses
`CNET_SOURCE_TEST_BIN` and the C harness accepts one library path argument.

Linux-only native process/filesystem path was exercised. The five facts are
exhaustively labelled within the certified sampled coverage; numeric 5–7 and
unrecognized named intents are not covered. Receipts prove literal-line checks,
not compiler/test results or source authenticity. The extractor does not
evaluate conditional compilation or general prose. The fixed tool/environment
is operator trusted. SHA256 and existing unkeyed manifest checks are integrity,
not signatures.

Source roots are reopened at each validation. Owner-controlled request-time
source stability is still required: the module does not provide an atomic
multi-file source snapshot, defeat same-owner malicious writers/ABA changes,
or guarantee post-check freshness. Serving integration must enforce freshness
on each used hop and again before final rendering, and include asset identity
and decoder conflict checks in working-set publication. This module report does
not claim those separate daemon/lifecycle gates have completed.
