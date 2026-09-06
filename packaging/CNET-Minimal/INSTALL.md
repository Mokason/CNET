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
Owner configuration, live knowledge transfer and service replacement require a
separate reviewed deployment procedure.

The repository's legacy `scripts/deploy_cnet_minimal.sh` / `make
cnet_minimal_deploy` is not that procedure: it still relies on a latest pointer,
merges live data and restarts services. It is outside this example-package gate
and must not be used to deploy this repaired packaging workflow.
