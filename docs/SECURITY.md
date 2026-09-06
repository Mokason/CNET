# Security and trust boundaries

Security status is scoped to evidence, not a claim that the entire repository
or machine has no vulnerabilities. The September 6 review and tests are recorded
in [the GPU product report](../result/cnet_gpu_product_sequence_20260906.md).

## Open dependency advisory

The separate managed control plane resolves
`Microsoft.Data.Sqlite 8.0.11 → SQLitePCLRaw.lib.e_sqlite3 2.1.6`.
Its September 6 transitive audit reports High-severity
[CVE-2025-6965 / GHSA-2m69-gcr7-jv3q](https://github.com/advisories/GHSA-2m69-gcr7-jv3q).
This is the SQLite aggregate-term overflow fixed in SQLite 3.50.2; the old
native-package family has no patched version listed in that advisory.
Security release clearance remains WITHHELD.

The main `dotnet/CNET.slnx` audit does not cover this separate project.
Ingestion and activation accept a `--db` path. Parameterized row values do not
make an attacker-controlled SQLite schema safe; schema preparation remains a
conditional exposure. No exploit was demonstrated, and deployed file ownership
and permissions were not audited.

Remediation must select a maintained compatible native bundle or explicit
provider, verify the actual loaded SQLite version/fix, test JSON functions,
ingestion transactions and activation/resume, then rerun control-plane tests and
the transitive audit. Microsoft's
[custom provider guidance](https://learn.microsoft.com/en-us/dotnet/standard/data/sqlite/custom-versions)
describes the mechanism. Until remediation, use only protected trusted database
files. Do not suppress the advisory or claim an old-package bump fixes it.

## Certified knowledge boundary

Only independently sourced evidence may train a candidate. `verified_tool`
and `user_correction` are local provenance labels, not authentication.
Capsule hashes identify content; they do not prove who supplied the labels.

Imports verify complete inventories. Typed contracts, canonical execution,
coverage, historical consistency and budget checks must all remain enforced.
A missing guard, malformed artifact or uncertain plan refuses. Never reduce a
certification floor, disable coverage or use residual prose to disguise refusal.

Use owner-private queue, policy and candidate directories. Do not expose the
teaching/publication CLI as an unauthenticated network upload service. Coordinate
writers to the same base; atomic file replacement alone is not a transaction
across arbitrary independent writers.

## Core candidate boundary

A core checkpoint starts unapproved. Independent graph/shadow evidence and
complete serving-path history replay precede owner activation. Training workers
cannot approve themselves. Pinned requests retain immutable model/inventory
generations. Hash integrity is not approval, and rollback invalidates approval
against an older active generation.

## GPU worker boundary

The bounded Linux worker accepts only the compiled train/evaluate task.
Landlock limits filesystem mutation to GPU devices, `/dev/null` and its private
scratch directory. Sealed snapshot input, closed inherited descriptors, strict
IPC length/exit checks and seccomp restrict the process boundary.

Trusted HIP initialization occurs before the network/process seal; its threads
inherit filesystem restrictions and later receive TSYNC filters. The runtime
and GPU driver are trusted. Read access is not confidential isolation, and
neither total HIP RSS nor total scratch storage has a hard quota. A wedged
driver may delay reaping. This is not a service for hostile native programs.

## Other implementation limits found during documentation review

- [Web query access](CNET_WEB.md) forwards mutating daemon commands; it is not
  a read-only capability. Its startup no-auth bypass does not remove request
  Bearer checks.
- The [managed sample server](../dotnet/Llm/docs/SERVER.md) lacks built-in auth,
  TLS and rate limiting. Keep it trusted-user/loopback only.
- [Managed scriptlets](../dotnet/Cce.Llm/README.md) run in-process; a timeout
  abandons the thread without terminating it. This is not hostile-code isolation.
- [Distillation proposals](CNET_DISTILL.md) may be written with an unchecked
  anti-collapse probe. Such proposals must not become training/admission input
  without independently establishing provenance and Tier-A exclusion.

These limitations are documented, not fixed by this cleanup. No exploit audit
of the entire managed/vendor tree is claimed.

## Focused rechecks

```sh
make capsule_demand_security capsule_acquire_security
make knowledge_capsule_sanitize
make -C experiments/offline_controller worker-sandbox-test product-test
dotnet list dotnet/CnetControlPlane/CnetControlPlane.csproj package --vulnerable --include-transitive
```

Package audits need restored assets and network access to advisory metadata.
GPU tests require the stated AMD/Linux environment. These commands do not
constitute an exhaustive OS/driver audit or public-release clearance.

Record findings privately with source identity, reproducer, affected boundary
and explicit remaining uncertainty. Follow [release policy](RELEASE_POLICY.md)
before publishing code, data, reports or vulnerability details externally.
