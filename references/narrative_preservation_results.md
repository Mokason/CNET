# Narrative Preservation Results

## 2026-07-06 Initial Synthetic Gate

This is an initial Phase 3 implementation result, not the full original-vs-compressed quality study.

Source reference: Narrative Flattening: How Post-Training Compresses Thematic, Affective, and Stylistic Variation in LLM Fiction, arXiv:2605.27878. The paper reports that post-training compresses thematic transitions, affective intensity, and stylistic diversity in fiction continuations. CNET's first response is a cheap native rubric that tracks four operational dimensions: voice consistency, moral ambiguity, delayed consequence, and folklore texture.

## Test Command

```sh
make narrative_coherence_test
```

## Result

Passed. The synthetic textured continuation scores above the flattened continuation by more than the required separation margin, passes the default contract, and yields lower flatness risk. The flattened continuation fails the default contract and reports missing folklore texture.

The same target verifies that task-aware routing prefers a branch tagged `CCE_SPECIALIST_NARRATIVE` for creative/story prompts while preserving the default nearest-branch route when no task hint is used.

## Pending Full Evaluation

- Build a set of original model continuations and CNET-compressed continuations.
- Score with the native rubric.
- Add human review.
- Add LLM-as-judge comparison.
- Report degradation against the Phase 3 target of <=15% narrative quality loss.
