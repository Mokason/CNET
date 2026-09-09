# Learned intent proposer — partial-intent confirmation passed

## Latest: new candidate passed every original floor

After test-first structural partial-intent repair and **2426/2426** regressions,
the NEW frozen grammar-on hybrid passed its one NEW independent confirmation:
**75/80 ready** (38/40 upper, 37/40 lower), **22/24 clarify**, **24/24 abstain**,
**zero wrong ready**, **121/128 exact**. All 128 rows remain scored. Seven misses
are retained, including 0/2 quoted-space requests. Floors were unchanged.

[Complete current result](cnet_learned_intent_proposer_20260910/confirmation-v2/result.md).
This closes the synthetic grammar-on typed-proposal gate for this candidate and
cohort only. Grammar-off, deployment, training eligibility and broader product
acceptance remain WITHHELD. No ablation established incremental learned gain.
The experiment worktree remains uncommitted; no push or live mutation occurred.

The sections below retain the previous candidate's failed checkpoint; its
binary, source snapshot, corpus, receipts and score remain preserved.

## Historical confirmation-v1: failed clarification floor

The one allowed score of the unchanged frozen grammar-on candidate completed:
**123/128 exact**, **80/80 ready** (40/40 each operation), **19/24 clarify**,
**24/24 abstain**, **zero wrong ready**. Clarification required 22/24.
All five misses were abstentions where clarification was required.

See [the complete result and next repair](cnet_learned_intent_proposer_20260910/confirmation-v1/result.md).
The independent author/custodian/blind-review sequence admitted the full 128-row
corpus before scoring. The candidate and all scoring pins are unchanged.
All raw predictions, drafts, reviews and validation evidence are retained.
The corpus is now exposed; no second fresh score is available for it.

## measured_pass

- Freeze suite: 2403/2403, including 16 proposer tests.
- Source hashes unchanged after that run. Pins:
  - LearningIntentProposer.cs `341840982a34e10d5a8bb051b5d115533012bbf6ae367ecb4dc6d264cadabea7`
  - cnet-control.dll `92db20a37772db1a2e6ae6b35c6e0b2c31f151246caa2c9e960fee6ad2653d96`
- Working tree is uncommitted on `experiment/herdr-language-20260909` above `f78fc27`.

## withheld

- Fresh acceptance: the single independent confirmation failed 19/24 clarify
  against the unchanged 22/24 floor. Completed authoring/scoring is not acceptance.
- Grammar-off promotion.
- Live deployment.
- Training eligibility.

## Historical next step (completed by confirmation-v2 above)

Implement and verify structural partial-intent clarification in a NEW candidate,
preserving terminal domain/extra-action refusals. Then freeze it and obtain a NEW
independent confirmation. Floors stay unchanged: 0 wrong ready, 72/80 ready,
34/40 per operation, 22/24 clarify, 23/24 abstain. No grammar-off or live promotion.
