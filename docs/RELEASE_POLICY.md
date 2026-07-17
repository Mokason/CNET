# CNET Release Policy

## Current status

- Version `5.1.1` is the integrity-closure internal release candidate.
- Unless a file states otherwise, CNET-authored source code and documentation
  are licensed under `Apache-2.0` as recorded in the repository `LICENSE`.
- The repository, source archive, binaries, campaign evidence, tags, and release notes remain private.
- License selection does not authorize public distribution or a repository-visibility change.
- No public release, remote push, tag, package upload, or announcement is authorized by this policy.
- GitHub Actions is manual-dispatch only. Pushes and pull requests must not start paid CI automatically.

## Local release authority

The local fail-fast authority is:

```sh
make release_integrity
```

A release candidate is acceptable only when that umbrella returns zero, its full log contains no failure verdict, `git diff --check` passes, and the generated archive proves a reproducible extract/build/install/consumer round trip. The Make entry point delegates to strict Bash `tests/run_release_integrity.sh`; its authority test injects a failing `make` and requires immediate nonzero exit, no later target calls, and no PASS marker before separately proving the success path. A PASS string from a non-fail-closed wrapper is not release evidence.

## Artifact handling

- `VERSION` is the single release-version source.
- `dist/` is generated, retained locally, and ignored by Git.
- Campaign logs and poisoned/stale checkpoints are retained locally but ignored by Git.
- The versioned shared library remains tracked because existing repository policy already tracks `cnet.so`; release commits must refresh it from the accepted source tree.
- Digests required for provenance remain tracked; transient logs remain ignored.

## Publication blockers

Public distribution remains blocked until the owner explicitly authorizes all of the following:

1. Copyright holder text and any required `NOTICE` or third-party attribution.
2. A remote destination and branch/tag strategy.
3. A final public-facing security and secret scan.
4. A public CI budget and trigger policy.
5. Publication of a specific immutable release digest.

Apache-2.0 applies to covered copies received under `LICENSE`; GitHub visibility
and release authorization are separate controls. Until the remaining decisions
are recorded, the repository must remain private and no external publication
action is permitted.
