# CNET Release Policy

## Current status

- Version `5.1.0` is the confirmed internal release candidate.
- The repository, source archive, binaries, campaign evidence, tags, and release notes remain private.
- No public release, remote push, tag, package upload, or announcement is authorized by this policy.
- GitHub Actions is manual-dispatch only. Pushes and pull requests must not start paid CI automatically.

## Local release authority

The local fail-fast authority is:

```sh
make PORTABLE=1 ci
make priority_acceptance
```

A release candidate is acceptable only when both commands return zero, their full logs contain no failure verdict, `git diff --check` passes, and the generated archive passes `tests/test_release_package.sh`.

## Artifact handling

- `VERSION` is the single release-version source.
- `dist/` is generated, retained locally, and ignored by Git.
- Campaign logs and poisoned/stale checkpoints are retained locally but ignored by Git.
- The versioned shared library remains tracked because existing repository policy already tracks `cnet.so`; release commits must refresh it from the accepted source tree.
- Digests required for provenance remain tracked; transient logs remain ignored.

## Publication blockers

Public distribution remains blocked until the owner explicitly authorizes all of the following:

1. A public license and copyright holder text.
2. A remote destination and branch/tag strategy.
3. A final public-facing security and secret scan.
4. A public CI budget and trigger policy.
5. Publication of a specific immutable release digest.

Until those decisions are recorded, no public license is granted and no external publication action is permitted.
