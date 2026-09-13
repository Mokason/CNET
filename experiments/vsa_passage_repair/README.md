# Passage repair experiments

Read `result/cnet_vsa_passage_repair_20260913.md` before interpreting scores.
No experimental scorer is linked into the answer CLI. Production changes are
exact arithmetic optimizations only. They failed the historical 25 us requirement;
the subsequently approved millisecond budget is described below.

Run from the repository root with Python + NumPy and native GCC available.
`probe.py` compiles the canonical C sources into an ignored shared library and
loads the existing production lexicon/capsules read-only. Nothing here reseals.

```
python3 experiments/vsa_passage_repair/test_boost.py
python3 experiments/vsa_passage_repair/evaluate.py
python3 experiments/vsa_passage_repair/train.py
python3 experiments/vsa_passage_repair/learned_eval.py
python3 experiments/vsa_passage_repair/rescue_eval.py
python3 experiments/vsa_passage_repair/pair_eval.py
python3 experiments/vsa_passage_repair/verify_runtime.py
```

`evaluate.py` requires the frozen q400 source/judgments in var/claude_scratch,
production bin artifacts, var/distill negatives, and the saved baseline CLI at
var/passage_repair_20260913/baseline_cli. Cached negative lists are authoritative:
preserve their source hashes and order. The absolute99 and rerank controls were
one-off analyses; their raw records and hashes are retained, not standalone
commands in this directory.

Missing judgments and train labels call the already running local Mistral
endpoint on port 8092. Scripts do not start or reconfigure a model service.
Do not substitute a different judge and compare its numbers as the same run.

For the token-alignment experiment, run with a ROCm PyTorch environment:

```
.venv-unlimited-ocr/bin/python experiments/vsa_passage_repair/late.py
python3 experiments/vsa_passage_repair/learned_eval.py --late
```

PyTorch's AMD backend uses its `cuda` device spelling; the script explicitly
requires `torch.version.hip`. This builds 32 nearest neighbors from frozen
lexicon vectors and extends already labeled training features. Preserve the
pinned graph for exact comparisons: floating-point top-k ties may vary across
hardware. The Python dense presence matrix is only an offline diagnostic, not
a proposed serving-memory footprint or latency result.

`verify_runtime.py` compares answer decisions/passages against the saved binary
and separately reports warm timings. A false `meets_25us_limit` is a failed
requirement, even when relative speed improves. Avoid concurrent CPU benchmarks
when collecting timings.

## Updated latency budget

The user subsequently approved a 1 ms p95 target and 5 ms p99 ceiling for the
warmed local answer path. `answer_budget.json` and `verify_budget.py` record
and measure this policy; the earlier 25us comparison stays as historical data.
See `result/cnet_vsa_answer_budget_20260913.md` for the new baseline and the
first richer word-interaction candidate. Its standalone feature computation
fits within a few milliseconds but did not improve validation quality, so it
is not integrated. Reproduce with `test_interaction.py` and
`interaction_eval.py`; no new teacher labels or model downloads are required.
