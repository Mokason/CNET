# Release policy

Repository metadata in [VERSION](../VERSION) identifies version 5.1.1.
A version string or an earlier acceptance report is not fresh release clearance.

## Private status and licenses

Covered CNET-authored source/documentation uses Apache-2.0 under
[LICENSE](../LICENSE), unless a file/subtree states otherwise. Vendored
`dotnet/Llm` and the linked `CNET.Cce.Llm` bridge carry GPL-3.0-only
metadata; preserve their attribution and license files.

The repository, archives, binaries, campaign evidence, tags and release notes
remain private. License metadata does not authorize public distribution,
a visibility change, a remote push, a tag, an upload or an announcement.
GitHub Actions remains manual-dispatch only; do not introduce paid automatic CI.

## Local release authority

```sh
make release_integrity
```

The Make target delegates to strict
[tests/run_release_integrity.sh](../tests/run_release_integrity.sh).
It requires a clean tracked tree and fresh fail-closed checks, then an
extract/build/install/consumer round trip. Read the runner before execution:
it manages generated evidence and is not appropriate over unrelated dirty work.

A candidate needs exit zero, no failure verdict in the complete log,
`git diff --check`, and the required reproducible package evidence.
A stale PASS string or successful documentation gate cannot substitute.
The authority test injects failures and checks exact propagation and stale-log
replacement; never relax it to produce a green release marker.

The [known security advisory](SECURITY.md) remains open. Documentation cleanup
does not clear security release review.

## Artifact handling

`VERSION` remains the release-version source. `dist/`, live bases, caches,
journals, recovery backups and transient campaign logs are local/generated
state, not default release inputs. Required provenance digests remain tracked.

Follow the existing tracked-library policy when preparing an actual release:
a release must bind its accepted source to its native artifact. This
documentation-only cleanup neither refreshes unrelated binaries nor deploys
them. Preserve private runtime data and all frozen benchmark evidence.

## Publication decisions

Public distribution additionally requires owner decisions on attribution/
NOTICE, exact destination and branch/tag strategy, a final secret/security
scan, CI budget/triggers and the specific immutable release digest.
No such authorization is granted by this document.

Current navigation: [INDEX.md](INDEX.md). Generated claims:
`make claims` → [verified-today.generated.md](verified-today.generated.md).
Those claims must retain their source, gate and WITHHELD scope.
