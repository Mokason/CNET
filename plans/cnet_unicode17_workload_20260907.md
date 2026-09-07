# Unicode 17 explicit case-change workload

Status: implemented and verified in a disposable private deployment. See the
[results, including the regression-discovered ownership fix](../result/cnet_unicode17_workload_20260907.md).

## Decision and bounded contract

The owner delegated selection of the first real public-source learning workload.
Choose the Unicode 17.0.0 Character Database, not CNET-generated labels.
Two datasets expose explicit single-code-point uppercase/lowercase changes for
Latin-1 **code points** U+0000..U+00FF. Inputs are numeric code points, not UTF-8
bytes; outputs are numeric code points, potentially outside Latin-1.

`unicode17_upper_latin1` selects UnicodeData field 12 (zero-based), and
`unicode17_lower_latin1` selects field 13. Empty fields produce **CNET abstention**.
Unicode defaults these fields to identity; therefore this is an explicit-change
lookup, NOT full simple case conversion. No locale, text decoding, case folding,
normalization, multi-character mapping or arbitrary text acquisition is claimed.

## Implementation sequence

1. Record RED tests before implementing the bounded offline extractor. Pin the
   official full source and its exact first 256 rows, with license and provenance.
2. Emit existing `CNET_LOCAL_TABLE_V1` evidence; reuse `learning import` and the
   certified capsule lifecycle. No new runtime adapter or packaging system.
3. Independently extract the source fields in the managed integration test.
   Start a fresh private daemon, import both datasets, record actual uncovered
   demand, run acquisition/activation/probation within a short original budget,
   and exhaustively verify both datasets after acceptance.
4. Review adversarially, run regressions, and record exact outcomes and limits.

The offline extractor accepts only the two pinned source byte sequences and
does no networking, overwrite, installation, demand admission or self-labeling.
It writes one canonical table to stdout. Hash pins provide reproducibility,
not cryptographic publisher authentication; provenance depends on reviewed
official HTTPS acquisition and the trusted owner/checker.

## Operational boundaries

Do not modify existing service policy, code, ledger, run budget or GPU workloads.
The real-source run is a disposable, isolated integration deployment, not a
production rollout. Both datasets must coexist after separate accepted changes.
The short probation policy is a test configuration, not evidence of long-term
reliability. No certification floor is changed. Full-source reproduction remains
possible without committing the entire upstream database.

Broader text learning, useful GPU allocator gain, 72-hour acceptance, and
high-scale capsule measurements remain WITHHELD.
