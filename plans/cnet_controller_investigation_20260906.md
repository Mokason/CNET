# Controller failure investigation

Scope: research and isolated diagnostic experiments, not a serving change.

Preserve the original experiment and original test set. Use existing training
and validation data for development, then freeze a separately generated
confirmation set before evaluating a selected configuration. Never put teacher
distances, paths, reachability or answers into model input features.

## Questions and controls

1. Undertraining: record training-probe and validation loss, first-action accuracy,
   reachable completion, unreachable abstention and invalid proposals at 600,
   2,000 and 6,000 updates; extend promising development runs to 30,000 if useful.
2. Representation: compare original features, immediately compatible adjacency,
   and current/goal-aligned node permutations with exactly remapped labels.
   Same model, optimizer, sample order and budgets; no path computation in input.
3. Sequential supervision: independently vary start-only versus all nonterminal
   current states on training graphs. This is not yet on-policy DAgger.
4. Execution masks and imbalance: inspect class distributions and distinguish
   learned abstention from deterministic refusal. Test immediate-legality masks
   separately if earlier ablations justify them; never remove final verification.
5. GPU usefulness: compare identical CPU/GPU updates and numerical error. Profile
   our process, not unrelated live jobs. Distinguish kernel time, transfers,
   end-to-end wall time, warmup and shared-GPU interference. No hardware-setting
   changes or service pauses. Research residency, batching and matrix kernels.
6. Deployment path: inspect existing core and capsule interfaces. Floating-point
   checkpoints are uncertified candidates, not capsules. Capsule admission needs
   typed contracts, independent evidence, coverage, regression and compatibility
   checks using the existing format.

Development success is measured progress, not a pretext to claim certification.
For the new confirmation fixture, preregister a research hurdle of >=95% reachable
completion and >=95% unreachable abstention on each of three seeds, with zero
independently observed invalid acceptances. Missing this hurdle is reported;
floors are not lowered and no candidate is automatically deployed.

Deliver source, raw development/confirmation evidence, primary-source citations,
causal findings separated from hypotheses, and a concrete next product sequence.

## Development extension (before confirmation generation)

The fixed-data aligned/all-state model reaches approximately 94% validation
completion but starts overfitting. Test one data-diversity extension: fresh
independently labelled graphs, same aligned features, model and optimizer.
Streaming training excludes topology partition 0 of the fixed mixed-64-bit
effective-adjacency hash modulo 5, and excludes all validation effective graphs.
Confirmation uses only partition 0 and excludes every original fixed split's
effective graphs. This prevents even differently encoded copies of a training
graph entering confirmation. Streaming is a new data regime, not an equal-data
architecture comparison. Maximum 30,000 updates; seeds 1,2,3 remain fixed.

Select the final 30,000-update streaming checkpoint only if it meets both
validation floors on all three seeds; otherwise retain the best measured
development configuration and report that development hurdle missed. Freeze the
selection and evaluator hashes before generating 2,048 confirmation cases.
Evaluate the same frozen candidate with/without the immediate-legality mask,
and original recurrent4/600-update weights on that same confirmation set.
No additional retraining or configuration selection on confirmation results.

Review correction before any confirmation generation: the raw-ID topology hash
could put node-renamed copies of the same aligned input across the split. Variant
6 (`aligned_stream`) is therefore **rejected as confirmation evidence**. Retain
its curves as development history. Variant 7 (`aligned_structural_stream`) uses
the sorted multiset of eight (out-degree,in-degree) pairs, FNV-1a hashed, modulo 5.
This assignment is invariant under every node permutation; coarseness groups
some nonisomorphic graphs together too. Reject all validation degree signatures
during training. Confirmation is partition 0 of this structural signature,
still excluding original fixed effective graphs. This is a structural holdout,
**not an IID sample** of the unconditioned generator and not a graph canonicalizer.
The same selection/floors/30,000-update budget apply to variant 7 instead of 6.
