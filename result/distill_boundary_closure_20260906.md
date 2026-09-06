# Distillation proposal boundary — 2026-09-06

Result: **25 private-process tests passed, zero skipped**, including the real
native front-door probe against freshly seeded private packs. The same 25 tests
passed with AddressSanitizer, UndefinedBehaviorSanitizer and leak detection.
This proves the measured proposal boundary; synthetic teacher fixtures do not
prove task acquisition, teacher correctness, certification or admission.

Distillation source baseline: `a419ce145bd165a91ba49af4562ec4b1a0a7f80f`.
Changes are the owned source/tests/docs in the commit containing this report.
Platform: Linux x86_64; compiler GCC. No live service, live pack root, production
registry, GPU or external teacher was used. Native packs came from
`roe_daily_packs_seed --root PRIVATE/packs`. Fake teacher executables were
isolated under each test's temporary directory. A private loopback listener
served only as a connection trap; the native probe never contacted it.

## Decision

Distillation requires exact, completed coverage receipts before calling a teacher.
Every query is checked first; any failed query refuses the entire batch before
teacher execution or publication. `LOCAL` queries receive an empty skipped row
and are excluded from candidate gold. Only `UNCOVERED` queries reach the teacher.

The native `roe_front_door probe QUERY --root OWNER_ROOT` command shares ordinary
ask preparation and routing but returns before network attachment, miss logging,
thought emission or promotion. It emits one fixed receipt, never echoed query
or answer text. It refuses mutation flags, malformed/ambiguous routes and catalog
load errors. A missing selected pack refuses. Optional absent always-on packs
retain ordinary ask inventory semantics; a nonempty loaded inventory is required.

The canonical process helper gained a bounded capture variant, preserving
existing callers. Receipt success requires complete stdout, no NUL bytes, EOF
and exit zero within a monotonic deadline. Failures kill the spawned process
group and reap the child; silent truncation is not accepted. Curl is invoked
through argv with config files and inherited proxies disabled. Complete response
syntax and the required completion schema are validated through the existing
shared JSON reader. Nonzero exits, schema/JSON errors, duplicate authority fields,
empty/truncated completions and oversized responses refuse without placeholders.

All replies remain in bounded memory until the whole batch succeeds. Publication
uses descriptor-relative output traversal, exclusive files in a unique private
directory and an atomic no-replace link for `PROPOSE.json` after row files and the
pending manifest finish. Created ancestors and final proposal/parent directories
are synced before success. After the manifest link, any failed sync retains all
complete files and reports `DISTILL_SLICE_COMMIT_UNCERTAIN`, exit 3, without PASS.
Consumers require the manifest. Symlink output paths,
unsafe domain identifiers and writable-by-others output directories refuse.
Existing proposals are not overwritten. Dry-run produces no files or subprocess
calls. The `--no-collapse-check` bypass and invented default queries were removed.

The owner configures probe executable, pack root and teacher endpoint/model;
these values are not model-selected authority. Plain HTTP is documented only for
trusted loopback use. Every proposal and candidate-gold row remains pending
verification and `auto_cert=false`. This slice does not modify any certification
or coverage floor and introduces no new package format.

## RED evidence

The parent recorded the original run before production changes in
`/tmp/cnet-distill-boundary-red-20260906.log`: **5 tests, 8 failures**. Dry-run
published files and probed; missing/failed/malformed/nonzero probes still
published; the bypass permitted teacher publication.

Additional tests ran against the original distillation binary before its
replacement: **4 tests, 12 failures**. Failed and malformed teachers still
published, input bounds/missing files were ignored, a symlink output was written,
and the whole batch was not checked before teacher execution.

An intermediate native probe then failed **1 test with 2 subcase failures**:
duplicate route fields and embedded NUL data were accepted. Both were repaired
before the final native build.

A supplemental baseline replay compiled the old front door from `a419ce1` into a
private directory. Two new native tests failed **5 assertions** because the old
binary did not implement the machine probe command. This replay occurred after
implementation and verifies that the new tests distinguish the baseline; it is
not represented as the initial RED-first run.

Parent review prompted additional failing tests before the corresponding repairs:
publication-sync injection produced **2 tests / 3 failures** with
`DISTILL_PUBLICATION_SYNC_RED` and `DISTILL_PUBLICATION_UNCERTAIN_RED`; closed
stdin produced **1 failure** with `DISTILL_CLOSED_STDIO_RED`; a catalog row with
an embedded NUL produced **1 failure** with `DISTILL_BINARY_CATALOG_RED`.
Fault injection is compiled only into private `CNET_DISTILL_TESTING` binaries;
production binaries contain no runtime fault-injection environment branch.

## GREEN evidence and commands

The ordinary warning-clean build and boundary suite executed:

```sh
cc -std=c11 -Wall -Wextra -Werror -O2 -D_POSIX_C_SOURCE=200809L -Iinclude -include include/cnet_platform.h -o bin/cnet_distill_slice tools/cnet_distill_slice.c
bin/cnet_distill_slice --selftest
make bin/roe_front_door
python3 tests/test_distill_boundary.py
make distill_slice
git diff --check
```

Results: `DISTILL_SLICE_SELFTEST_PASS`; **25 tests passed**, no skips;
all commands exit zero. The tests cover:

- No-effect dry-run and removal of the bypass.
- Missing/failed/malformed/nonzero/extra-output probes; exact per-query receipt
  interpretation; all probes precede teacher requests.
- Native LOCAL and UNCOVERED results, alias preparation, query text containing a
  fake receipt, and unchanged private-root files with live flags enabled.
- No connections to a private listener configured as both network endpoints.
- Native Tier-A exclusion from gold, mutation-flag refusal, corrupt catalogs,
  duplicate route fields and binary route data.
- Teacher process errors, wrong schemas, empty/trailing/duplicate JSON fields,
  non-completed generation, response bounds, timeouts and late-batch failure.
- Model/query shell metacharacters passed as argv/JSON data, with UTF-8, quotes,
  tabs, backslashes and newlines preserved in output.
- Query count/length/file bounds, missing files, malformed miss logs, NUL data,
  symlink/unsafe-permission output refusal, unique proposals, closed-stdio
  refusal and killed descendant processes.
- Publication failure before the commit marker leaves no manifest; failures
  after the link and during parent sync retain complete rows/manifest and report
  an uncertain commit. Fault-test binaries are instrumented in sanitizer runs.

Sanitizer artifacts were built under `/tmp/cnet-distill-sanitizers-i8a2bZ`.
The distillation binary used the same compiler flags with `-O2 -g
-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie`.
The native front door used `-O2` with those sanitizer/debug flags and its existing
Make source list/link libraries. Both retained `-Wall -Wextra -Werror`.

```sh
CNET_DISTILL_TEST_BIN=/tmp/cnet-distill-sanitizers-i8a2bZ/cnet_distill_slice CNET_FRONT_TEST_BIN=/tmp/cnet-distill-sanitizers-i8a2bZ/roe_front_door ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 python3 tests/test_distill_boundary.py
```

Result: **25 tests passed**, no sanitizer findings, exit zero. An earlier native
`-O1` sanitizer build failed on existing `format-truncation` diagnostics in
`cnet_roe_asi.c` and `cnet_probe_shortcircuit.h`. Clang was unavailable. Using the
native target's production `-O2` optimization completed the warning-clean
sanitizer build; no warning or audit suppression was added.

## Bounds and review

The parent approved the boundary design before implementation, reviewed the
publication/process/catalog corrections, and approved the final scoped commit.
Security, incremental implementation,
debugging and review skills kept process execution, input validation and
publication checks explicit and separated RED evidence from acquisition claims.

Not measured: real teacher quality, mutable-inventory snapshot consistency,
admission of candidate gold, process/power-loss durability of proposal directories,
other operating systems or live deployment. A stable owner-selected inventory is
required for each supervised batch, with no concurrent catalog mutation between
probe text preflight and catalog reload. No immutable pack snapshot is claimed.
The parent updated the Make recipe to run
this private suite; `make distill_slice` passed with `DISTILL_SLICE_PASS`.
Broader integrated product gates remain separate from this scoped result.
