# Evidence operator publication validation — 2026-09-11

Validated in a clean detached worktree based on origin/master
`3cb14bf1572b54a8395f48f949fb85a3f68d1d3b`, with only the evidence operator,
its CLI integration, tests, experiment harness and measurement records overlaid.
The unrelated uncommitted format-v3, q8 optimization and crawler changes were
excluded from publication. The operator has no dependency on those changes.

The measurement adapter explicitly supports the older master encoder API and
records `response_encoder_profile=legacy_float`. The original working-tree
measurement is preserved separately; it must not be confused with a measurement
of only committed master sources.

Clean-master results on the original frozen fixtures:

| Suite | Baseline correct | Candidate correct |
|---|---:|---:|
| Controlled language, including refusals | 224/480 | 416/480 |
| Bounded procedure acquisition, including refusals | 288/480 | 480/480 |
| Supported responses and refusals | 2/96 | 96/96 |

The grammar/procedure candidates remain experimental for the reasons in the
original report. The native response operator retains 32/32 supported answers
and 64/64 refusals. Its median native timer interval is below 1 µs; the older
master n-gram baseline is approximately 482 µs. These are bounded task results,
not evidence of broad transformer superiority, which remains WITHHELD.

Validation passed:
- `make cnet_vsa_evidence_bench`: native API, CLI malformed-input/refusal tests,
  and 4,096 comparisons on 2,048 disjoint random graphs, including 341 accepted
  proof checks.
- `make cnet_vsa_cli_bench cnet_vsa_gencap_bench cnet_vsa_router_bench`.
- `make verify-fast`.
- AddressSanitizer and UndefinedBehaviorSanitizer on the native evidence tests.
- Experimental grammar, procedure and response sanity checks; full measurement
  rerun with frozen fixture SHA-256 matching the original run.

The older router gate initially failed its `count >= 10` precondition in an empty
worktree. It passed after copying the five capsules named by that test and five
additional technical-domain capsules from the existing local fixture collection.
No test assertion or certification floor was changed. File identities are in
`cnet_evidence_router_fixtures_20260911.json`; these local binary fixtures are not
published. The gate still requires its existing local fixture prerequisite.

The new C API and CLI implementation were reviewed for input bounds, signed-fact
semantics, proof completeness, no partial output, memory use and preservation of
ordinary routing. No runtime capsule format or certification policy is changed.
The original footprint comparison (4,643 additional CLI section bytes) describes
the original working-tree build, not a fresh binary-size claim for older master.

Reproduce the clean-master measurement:

```sh
bash experiments/hdc_capabilities/build.sh
python3 experiments/hdc_capabilities/run.py --out result/cnet_hdc_capabilities_master_20260911
make cnet_vsa_evidence_bench
```
