# CNET-Minimal native example runtime

This package contains freshly seeded example packs plus six fixed reviewed gold
examples in `config/coverage_gold.tsv`. It does not copy learned, personal or live
repository data. Packaging is not a live migration or a deployment operation.
ROE text packs are distinct from CNU1 certified capsules and MTK weight deltas;
the fixture's LOCAL/CERT labels do not establish new capsule certification.

## Build and inspect

From the repository, run `make cnet_minimal_package`. Required native binaries
are built as prerequisites; directly invoking the packager requires those
binaries to exist already. A missing binary, failed harvest or failed smoke
refuses `PACKAGE_OK`. The package includes the actual native `roe_evolve_tick`
gardener and `roe_gold_put`, not only their gate executables.

Default outputs are `dist/CNET-Minimal-<git-short-id>` and its `.tar.gz` sibling.
Select explicit new paths with `CNET_MINIMAL_OUT` and `CNET_MINIMAL_TAR`, or use
`CNET_MINIMAL_VERSION` for a fresh name. Existing paths, symlink ancestors,
overlapping output/archive paths, single quotes and control characters are
refused. No existing output is removed and no latest-path pointer is updated.
A failed build can retain its new partial directory for inspection; only
`PACKAGE_OK` reports successful archive creation.

The package requires Linux, Bash, GNU core utilities (including `realpath`,
`timeout`, `sha256sum`), `awk`, `grep`, `find`, `sort`, `xargs`, `tar`, `gzip`,
`jq`, and any shared libraries linked by the native binaries (such as libcurl
when built with it). It does not include a Python runtime or Python helpers.
Build-machine binaries are not a promise of compatibility with every host.

## Test an extracted copy

Extract the exact selected archive into a new private directory. From its root:

```sh
sha256sum -c MANIFEST.sha256
bin/cnet-ask 'who are you'
bash scripts/cnet_runtime_smoke.sh
bash scripts/cnet_runtime_soak_gate.sh
```

Absolute invocations also work from outside the package. `CNET_MINIMAL_ROOT`
can explicitly select the package root for smoke/soak. `cnet-ask` forces offline
settings and resolves its own package paths; OOD asks append demand to the
package's miss log. This normal runtime write changes the packaged miss-log
checksum. Keep an untouched extracted copy when verifying archive integrity.
The checksum manifest detects changes; it is not provenance authentication.

Smoke/soak always work on newly allocated private copies in `/tmp`, and print
their artifact paths. Smoke requires nine verified LOCAL answers, exact answers
for the six gold examples, nine CERT dispatches, four explicit OOD abstentions,
the native blocklist selftest, a native gardener dry-run, the chain skeleton
trace and the stream-index fixture benchmark. Missing components and failed
commands fail even when their output contains a success marker.

The gardener dry-run may write reports on its private copy; the test checks
that admitted catalog content and evolution state do not change. Soak performs
two real native gold-only gardener ticks on its private copy, requires blocklist
skips and correct state advancement, repeats the nine LOCAL/four OOD probes per
tick, and verifies the six-row personal catalog remains unchanged.

These are bounded offline fixtures. They do not test live teachers/reviewers,
new task acquisition, autonomous cycles, a production local-hit floor, prolonged
resource stability, base-model portability or public-release readiness. Those
claims remain WITHHELD. The package contains no teacher/reviewer credentials.
Owner configuration, live knowledge transfer and service replacement require
separate target approval and fresh operator checks.

## Explicit private deployment handoff

The repository's `scripts/deploy_cnet_minimal.sh` now accepts only an explicit
archive, a separately trusted SHA-256 digest, and a **new** destination. It does
not build an artifact, select a latest pointer, copy live/personal knowledge,
merge configuration, switch symlinks, start services or change timers. The
flagless `make cnet_minimal_deploy` entry point refuses; use the explicit CLI.
`make cnet_minimal_deploy_gate` exercises private fixtures, not a live rollout.

The deployment host additionally needs `/usr/bin/python3` (Python 3 stdlib,
tested with 3.12.3), Linux `/proc`, and libc/kernel/filesystem support for
`renameat2(RENAME_NOREPLACE)`. This dependency is only for the repository
handoff validator; the extracted runtime remains native/Bash as above.

Choose an owner-controlled artifact and obtain its expected archive hash from
a trusted release/build channel independently of the artifact bytes. Neither
the embedded manifest nor a hash calculated from an arbitrary download
authenticates its author. A successful non-dry invocation runs packaged native
executables and scripts with the invoking user's authority: this is not an
untrusted-code sandbox. Do not run this helper as a privileged service account.

Use absolute, symlink-free paths with ASCII letters, digits, `_`, `-` and `.`
components of at most 96 bytes. The destination must not exist and its existing
parent must be owned by the invoking user with no group/other permissions
(normally `0700`). Ancestors must be root- or invoking-user-owned and must not
be group/world writable except sticky directories such as `/tmp`. Root, home
and workspace destinations are refused; no destination within this repository
or any path with an ancestor `.git` marker is accepted. Place the archive in an
owner-controlled directory satisfying the same ancestor restriction; it must
be an owner-owned, regular, single-link file. Do not change checkout permissions
to satisfy these rules: select a private handoff directory outside it.

For example, after selecting real paths and an independently trusted 64-hex
digest (the values below are placeholders):

```sh
bash scripts/deploy_cnet_minimal.sh \
  --artifact /home/owner/cnet-handoff/CNET-Minimal-release.tar.gz \
  --sha256 TRUSTED_64_HEX_ARCHIVE_DIGEST \
  --destination /home/owner/cnet-deployments/release-20260906 \
  --dry-run
```

`--dry-run` reads and validates the archive, manifest and destination boundary;
it creates no files or directories and executes no packaged code. It does not
prove host binary compatibility. Only after checking that result, repeat the
same explicit command without `--dry-run`. Nothing is deployed by the examples
in this document.

The validator permits the current single-root native package layout only. It
rejects missing/extra files, incomplete status, manifest mismatches, path
traversal, duplicate names, links, special files, PAX/sparse extensions and
trailing gzip/tar payloads. Supported GNU long-name records remain bounded.
Limits are 32 MiB compressed, 128 MiB expanded, 32 MiB per file, 4096 members,
20 path components and 2047 bytes per member name. Unsupported packages fail
loudly; there is no skip-verification flag.

A real invocation extracts through pinned directory descriptors into a fresh
same-parent `0700` staging directory, verifies every byte, runs the offline
native fixture smoke (60-second total, 256 KiB output cap), and rechecks all
files and permissions. A 120-second deadline alarm guards validation/handoff
after interpreter startup; cleanup and uninterruptible kernel I/O are not a
hard wall-time guarantee. The successful smoke's separately allocated private
`/tmp/cnet-runtime.*` diagnostics are
retained and their path is printed. The helper then atomically publishes with
no replacement and syncs the deployment and parent directories. Existing
deployments remain untouched. The owner must not concurrently mutate the
selected artifact, parent directory or staged deployment during this workflow.

Exit `0` with `CNET_MINIMAL_DEPLOY_PASS` means the new private fixture deployment
was published; `live_switch=0` always applies. A failure before publication
removes only this invocation's new staging directory. Exit `3` with
`CNET_MINIMAL_DEPLOY_COMMIT_UNCERTAIN` means publication occurred but the final
durability check failed, hit its deadline, or was interrupted by SIGINT/SIGTERM:
the complete new deployment is retained. Do not delete it or report rollback.
Inspect it and storage health
before deciding what to do; a retry at the same destination will refuse.

Rollback here means the owner manually selects an earlier untouched deployment
directory for a separately approved consumer. This helper never redirects a
consumer and has no automatic rollback or destructive cleanup command. A live
rollout, service account/configuration migration, operational health check and
service-level rollback remain WITHHELD until an exact target is approved.
