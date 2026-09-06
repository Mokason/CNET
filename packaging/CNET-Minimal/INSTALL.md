# CNET-Minimal package operations

Build a private package from the repository with:

```sh
make cnet_minimal_package
```

Inspect the generated `dist/` artifact and its manifest before using it.
Package creation is not permission to install it, replace a live base, restart
services or publish the archive.
The packager removes/rebuilds its selected output directory. Never point
`CNET_MINIMAL_OUT` at a live installation or a directory containing other data.

## Test an extracted copy

Extract the exact chosen archive into a new empty private directory.
Do not use an ambiguous wildcard or unpack over an installed runtime.
Inside that extracted package, set `CNET_MINIMAL_ROOT` to its absolute
root and run the included `scripts/cnet_runtime_smoke.sh`.

The packager copies available front-door binaries, packs, configuration and
helper scripts, but still has a legacy gap: it attempts to copy the removed
Python `tools/roe_evolve_tick.py` and does not copy `bin/roe_evolve_tick`.
An evolution gate binary is not the gardener. Do not claim a complete packaged
evolution workflow from `PACKAGE_OK`; that packaging path needs a separate fix.
Some helper paths still require Python or optional external services;
do not describe every packaged workflow as dependency-free or offline.

## Installation boundary

Review effective socket, pack, queue and governor paths. Preserve live
knowledge and operator configuration; coordinate writers before replacement.
Teacher/reviewer credentials remain external to the package and version control.
The packager copies runtime pack content; it is not a secret scrubber. Inspect
personal/private material before moving even a locally generated archive.

A smoke pass only names its fixture. `make cnet_runtime_soak_gate` is a
separate gate, not proof of all real traffic or public-release readiness.
MTK cartridges remain residual tensor deltas; pack text and CNU1 capsules
have distinct acceptance rules. No packaging operation grants certification.
