# Source checkpoint — 2026-09-06

## Scope

Publish the accumulated native core, serving-boundary, acquisition, checkpoint,
capsule-capacity, build, .NET interface and regression work together: the
4,096-capsule changes depend on the earlier uncommitted core implementation.
Existing measurements are dated evidence, not a claim about the running daemon.
No service restart, live knowledge mutation or controller deployment is part of
this checkpoint.

## Review boundary

Private environments, models, runtime memory, generated binaries, external
datasets and unrelated result dumps remain local. The following experimental
helpers also remain local, along with their Makefile/service enablement:

- Discord delivery/bridge and unattended improvement helpers: ambiguous message
  destination, token-in-process-arguments and private installation dependencies.
- RSI live proof: removes a live brick without guaranteed restoration.
- Improve-info live gate: sends fixture notes to an existing daemon's knowledge.
- Private Python mouth-training/serving and morphology acquisition experiments.

Their files are preserved; exclusion is not deletion or a completed repair.
There is no assertion that the historical repository-wide Python audit is green.

## Fresh checkout fixes

`tests/test_capsule_fresh_build.sh` first emitted `CAPSULE_FRESH_BUILD_RED` for
the missing daemon file target, then for the missing flagship archive dependency.
Both were corrected. The standalone capsule tool now creates bin/log directories.
Executable modes for newly added shell programs and fake tool fixtures are
recorded in Git, not merely present in the local filesystem.

The exact staged source is exported and verified outside the live installation.
Raw historical logs retain their original whitespace and checksums. A successful
push publishes source only; resident capsule swapping and the neural-controller
experiment remain separate work.
