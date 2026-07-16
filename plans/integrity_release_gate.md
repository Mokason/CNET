# Integrity Slice — Release Artifact Authority

**Place:** `tests/test_release_package.sh`, `Makefile`, generated evidence.

**Dilemma:** package smoke validates the checkout and only lists two tar members; release policy requires several manually composed commands.

**Consequence:** an incomplete archive or omitted gate can still look releasable.

**Systematic component:** deterministic archive, clean extraction, build/install/consumer execution and exact markers. **Noise:** compiler binary bytes; source archive reproducibility is strict, produced shared-library identity is recorded rather than assumed byte-identical across toolchains.

## TDD

1. Add a fixture proving the current package test never compiles from extraction.
2. Extract the generated tarball into an empty root, build `cnet_dll`, stage-install it and execute a pkg-config consumer from that extraction.
3. Build the source archive twice and require identical SHA-256.
4. Add `release_integrity` as the only final authority: focused integrity slices, portable CI, priority acceptance, diff hygiene and archive self-test.
5. Capture final output under `set -eo pipefail`; require `CNET_RELEASE_INTEGRITY_PASS`.

## Focused Evidence

- The v5.1.1 source archive is byte-reproducible across two builds.
- A clean extraction builds `cnet.so`, stages headers/library/pkg-config metadata, links an external consumer through `pkg-config`, and executes it.
- `release_integrity` is statically audited for ordered focused slices, one archive build, portable CI core, priority acceptance, diff hygiene and a clean tracked integration tree.
- Final umbrella execution remains the closure authority; this section is not a substitute for `CNET_RELEASE_INTEGRITY_PASS`.
