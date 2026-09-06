# Phase 1–3 evidence taxonomy

`make phase123_benchmark_test` runs native contracts and claim-integrity
checks. The [benchmark runner](../tools/run_phase123_benchmarks.c) distinguishes:

| Verdict | Meaning |
| --- | --- |
| `contract_pass` | Local API/fixture contract passed |
| `measured_pass` / `measured_fail` | The named metric ran on its required path |
| `withheld` | Prerequisites or measurement are absent |

## Scope by phase

Counterfactual route evidence can be attached in report-only mode without
changing served answers. That integration is not a factuality improvement
measurement. The retained [TruthfulQA dataset record](../references/truthfulqa/README.md)
does not itself supply a scored model A/B.

Sparse KV has native selector and synthetic attention-path checks.
These do not establish LongBench/InfiniteBench quality or imply that a
separate admitted llama.cpp runtime executes the same selector.

Narrative preservation has a bounded lexical-rubric comparison on two
prompts. Its recorded pass is not human preference or independent model
judging. See [preservation evidence](../references/narrative_preservation_results.md)
and [rubric](../references/narrative_coherence_rubric.md).

The previous detailed closure report is retained exactly in the
[documentation archive](MAINTENANCE.md). Its
`PASS_WITH_EXTERNAL_CLAIMS_WITHHELD` is a scoped historical verdict,
not permission to claim the withheld external benchmarks passed today.
