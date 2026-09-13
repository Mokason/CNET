# Answer quality budget — 2026-09-13

The user approved a new budget: 1 ms target, 5 ms p99 ceiling for the warmed
local answer path. This supersedes the earlier 25 us requirement and the
intermediate 100 us discussion. It is not a relaxation of certification floors.
Use p95 <= 1 ms as the target measurement and p99 <= 5 ms as the ceiling.
Measure the complete route, evidence selection, verification and answer
assembly; report cold loading separately. Also record max and CPU time.

Serving remains CPU-only, with one worker in the experiment and no nested
thread pools. Request rate determines aggregate CPU load; no latency number
alone proves absence of spikes. Model/table size must be reported explicitly.

Quality qualification requires more correct answers AND higher correct-2*wrong
than the existing baseline. Preserve all certification floors and refusal
checks. A candidate's cross-capsule calibration must not be confused with
answer sufficiency. Evaluate on capsule-disjoint validation first; the old 400
are an exposed regression set. Fresh question judgments are required before
claiming a new independently demonstrated quality improvement.

First bounded experiment: richer word-interaction evidence features inspired
by K-NRM (https://arxiv.org/abs/1706.06613), with frozen CNET lexicon vectors,
a compact projection and the existing small tree learner. This is not an
implementation or reproduction of the paper's end-to-end-trained model.
Compare to the previous 12-feature learner on the same teacher labels and
capsule-disjoint validation. No transformer executes online. Stop before
production integration if validation does not improve. The additional budget
creates room to test better models; it does not certify a quality gain.

First experiment completed: feature computation p99 1.126 ms, mean thread CPU
.591 ms, projection vectors 5.05 MB. On the existing 163-capsule validation,
37 features yield 40 correct / 11 wrong versus the prior 16-feature learner's
42 / 11. Qualification fails; no production integration or reseal. The current
production path already meets the revised warm budget (p95 .0544 ms,
p99 .0657 ms). Record: result/cnet_vsa_answer_budget_20260913.md.
