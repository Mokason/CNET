# Toolchain digest: linked-runtime attestation (scope)

Status: IMPLEMENTED, slices 1–2 (2026-07-17) — `runtime_libs_digest` is an
ABI-safe tail on `CnetOracleIdentity`, computed in `src/runtime_identity.c`
(`dl_iterate_phdr` build-id fold + glibc version), persisted in CNB v5 with
v1–v4 read compatibility, populated by the daemon, and projected exactly with
the existing full artifact SHA-256 through native `SoulHost`, .NET, and MCP.
The original `soul_oracle_identity` ABI is unchanged; new independent accessors
carry the full hash/runtime record; the managed descriptor adds optional zero
defaults and retains the exact eight-argument overload so source and binary
callers remain valid. Permanent gates:
`make base` (94 checks), `make gap_lane` (51), `make soul_host_test`, focused
managed/MCP real-fixture tests, and `make unified` symbol + exact-value checks.
Only GPU driver/kernel folding remains deferred; it applies only when a GPU
lane teaches and must not perturb CPU-only identities. Original scope below.

## What the digest covers today

`lm_toolchain_identity()` (tests/gap_lane_run.c) FNV-folds: compiler
version string, `CNET_TOOLCHAIN_CFLAGS`, `CNET_SOURCE_REV` (both
Makefile-injected), `CNET_ORACLE_ABI_VERSION`, and pointer width. A
zero digest reads `unattested` and is refused as provenance
(reconcile_one, src/gap_lane.c).

## The hole closed for CPU teaching by slices 1–2

Before CNB v5, teaching numerics also depended on code the toolchain digest did not see:
- glibc/libm (expf/tanh differ across versions — logits shift),
- the OpenMP runtime (reduction/scheduling order),
- OpenCL driver + kernel binaries when a GPU oracle pool teaches.

A teacher rebuilt against a different libm can produce a different
exemplar table under an IDENTICAL current digest.

## Implemented mechanism

Add `runtime_libs_digest` (u64) to `CnetOracleIdentity` as an ABI-safe
TAIL extension — the exact precedent of `artifact_sha256` (July 13):
`struct_size`-gated, NOT folded into `cnet_oracle_identity_digest`
(the 64-bit digest stays the fast index; the new field is the record).

Computation at daemon startup, pure C:
1. `dl_iterate_phdr` over loaded DSOs; for each, read the
   `.note.gnu.build-id` ELF note; fold (soname, build-id) pairs
   sorted by soname. Covers libc, libm, libgomp, libOpenCL — whatever
   is actually linked, not a hardcoded list.
2. Fold `gnu_get_libc_version()` explicitly (build-id can be stripped).
3. GPU lanes only: fold `CL_PLATFORM_VERSION` + per-device
   `CL_DRIVER_VERSION` at pool construction; CPU-only teaching folds
   nothing GPU (a CPU teacher must not change identity when a driver
   updates).

## Serialization + compat

CNB descriptor record gains the field → CNB v5 with v1–v4 read compat
(older descriptors load it zero = "linked runtime unattested", exactly
how pre-v4 loads read the full hash as all-zero). Ledger untouched.

## Gates

- `make base`: +2 checks (round-trip of the new field; zero-on-old-load).
- `make gap_lane`: daemon populates it.
- `make soul_host_test`: native exact-value and invalid-argument accessor
  checks over the real CNB fixture.
- Managed/MCP tests: exact 64-character lowercase `artifactSha256` and
  fixed-width lowercase `runtimeLibsDigest` reach `cnet_list_oracles`; legacy
  managed construction receives explicit zero defaults.
- `make unified`: requires the two exported accessor symbols and exact fixture
  values on the real stdio MCP response. The service config gate is also a
  permanent native prerequisite.
- A refusal is NOT added: a zero runtime digest stays a visible label,
  not a hard fail (same posture as `unattested` toolchain today).

## Cost / verdict

~60–100 lines C + serializer bump + gate rows; roughly a half-day.
Honestly modest for a single-host runtime (same caveat as the
artifact-hash closure) — but it is the one remaining named provenance
hole, and the mechanism is cheap and dlopen-order-independent.
