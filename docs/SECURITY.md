# Security and trust boundaries

Security status is scoped to evidence, not a claim that the entire repository
or machine has no vulnerabilities. The September 6 review and tests are recorded
in [the GPU product report](../result/cnet_gpu_product_sequence_20260906.md).

## Repaired dependency boundary

The separate managed control plane now uses `Microsoft.Data.Sqlite.Core 8.0.11`
with `SQLitePCLRaw.bundle_e_sqlite3 3.0.5`. The actual loaded SQLite version was
3.53.4; focused transaction/JSON/activation tests and all 80 control-plane tests
passed, with zero reported transitive package advisories in the recorded audit.
The prior native bundle loaded 3.41.2 and triggered
[CVE-2025-6965 / GHSA-2m69-gcr7-jv3q](https://github.com/advisories/GHSA-2m69-gcr7-jv3q).
See [the RED/repair/audit evidence](../result/sqlite_provider_closure_20260906.md).

The main `dotnet/CNET.slnx` audit does not cover this separate project.
Ingestion and activation accept a `--db` path. Parameterized row values do not
make an attacker-controlled SQLite schema safe; schema preparation remains a
conditional exposure. No exploit was demonstrated, and deployed file ownership
and permissions were not audited.

Continue using protected trusted database files. Repeat loaded-provider tests
and both project audits after changes; package locks are not permanent advisory
clearance. No suppression or dependency-floor reduction was used.

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

## Repaired product boundaries and retained limits

- [Web query access](CNET_WEB.md) forwards mutating daemon commands; it is not
  a read-only capability. Its startup no-auth bypass does not remove request
  Bearer checks.
- The [managed sample server](../dotnet/Llm/docs/SERVER.md) has separate bounded
  inference/admin bearer authority, exact-origin CORS, bounded requests/rates,
  one active model operation and safe errors. Protected operation requires an
  explicitly trusted loopback TLS proxy; development bypass is explicit and
  loopback-only. Generation cancellation is cooperative at token boundaries,
  not hard prefill/kernel preemption. Candidate model load failure retains the
  old model; failed retirement is loud and requires restart before another swap.
- [Managed scriptlets](../dotnet/Cce.Llm/README.md) compile and execute in a
  killable Linux worker. Landlock precedes CLR startup; seccomp is synchronized
  before source consumption. Privileged/unsupported launches refuse, with no
  in-process fallback. Input/output/time/address-space/heap limits and reaping
  are tested. The kernel, trusted runtime and installed artifacts remain trusted;
  this is not a universal .NET sandbox or a side-channel guarantee.
- [Distillation proposals](CNET_DISTILL.md) require complete successful native
  per-query anti-collapse receipts before teacher acquisition/publication.
  LOCAL answers cannot become labels. Dry-run does not publish. Digest integrity
  and a successful probe do not authenticate a teacher or establish correctness.
- [Resident capsule control](CAPSULE_CORE.md) is owner-only and separate from
  chat. Immutable snapshots, self-closure, historical identities and monotonic
  selection protect activation/recovery. Post-commit durability uncertainty
  freezes mutations; it must never be reported as rollback or success.
- [Source evidence](CNET_SOURCE_EVIDENCE.md) binds exact source/receipt/decoder
  bytes and checks current files at used hops and final emission. Concurrent
  malicious same-UID source writers and arbitrary compiler semantics are outside
  the supported boundary.

These are focused code repairs with negative tests and independent review.
No exhaustive exploit audit of the managed/vendor tree, OS or drivers is claimed.

## Focused rechecks

```sh
make capsule_demand_security capsule_acquire_security
make knowledge_capsule_sanitize
make capsule_product_sanitize
make capsule_product_closure capsule_control native_workflows_test distill_slice
make -C experiments/offline_controller worker-sandbox-test product-test
dotnet list dotnet/CnetControlPlane/CnetControlPlane.csproj package --vulnerable --include-transitive
```

Package audits need restored assets and network access to advisory metadata.
GPU tests require the stated AMD/Linux environment. These commands do not
constitute an exhaustive OS/driver audit or public-release clearance.

Record findings privately with source identity, reproducer, affected boundary
and explicit remaining uncertainty. Follow [release policy](RELEASE_POLICY.md)
before publishing code, data, reports or vulnerability details externally.
