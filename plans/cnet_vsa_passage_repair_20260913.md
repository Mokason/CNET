# Passage evidence repair — 2026-09-13

User requests an actually improved answer path; prior latency ceiling 25 us
remains. Existing 812-capsule route exceeds that ceiling; report it separately
and do not imply that a 5 us passage scorer makes the complete path compliant.

Baseline user changes are backed up and hash-pinned in ignored
var/passage_repair_20260913/initial*. Old experiment is superseded by this task.

Hypothesis: within-capsule best-versus-rest z loses evidence when several
passages are relevant. Compare absolute cosine + 0.2*normalized lexical overlap
against hybrid z, and VSA-only control, using the real seal negative sampler.
These settings are fixed before scores. Lexical weights come from the same
pinned production lexicon, not the Python scratch tokenizer/global IDF.

First experiment only changes normally accepted routes. Sibling path keeps
original VSA score, z floor and zgap; absolute scores cannot replace a z-gap
without fresh calibration. Radius/margin/term gates remain unchanged.

Qualification: RED tests; baseline reproduction; negatives from the exact
source/sampler; empirical rejection >= 90% including ties; 400 exposed questions
judged with the same local Mistral protocol, reuse identical cached judgments;
3994-question capsule correctness is only a routing diagnostic. Need more
correct answers and better c-2w, not precision alone. No new format or reseal
until an experiment earns it. Any production change requires digest-covered
config, exact legacy round-trips/refusals and real sealed registry validation.

Additional controls after the first failures: two-passage composition (top-eight
beam, require mutual cosine >= .15 and composite cosine gain >= .05) failed net
score. The absolute-statistic 90% floor accepted every normally routed question;
this exposes the easy cross-capsule-negative distribution, not answer confidence.
Test a stricter 99% rejection control with the same pinned lists and .2 weight.
This increases the rejection requirement; it is not a lower gate. These remain
exposed-set exploratory controls, not fresh certification.

Conservative rescue control: keep every v4 answer and every sibling decision
verbatim. For normally routed, floor-refused questions only, admit a learned
passage if its score exceeds BOTH the separate validation floor and the maximum
learned score of every pinned negative that v4 refused. This permits zero added
accepts on each original negative list, rather than spending a second 10% error
budget. It cannot lower or replace the original floor. Score this fixed policy
before deciding on any format or production change.

Outcome: no quality candidate qualified. Token alignment gives the same 126
correct answers with fewer wrong answers; the conservative union improves
correct coverage but has unchanged net utility. Retain only the exact integer
dot and incremental leave-one-out performance changes. Full output parity and
hermetic gates pass. Warm total p95 falls from roughly 83–84 to 50–57 us, still
above the 25 us limit. No format change, no reseal, no claimed coverage fix.
Record: result/cnet_vsa_passage_repair_20260913.md.
