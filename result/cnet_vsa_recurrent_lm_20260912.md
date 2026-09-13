# Recurrent language models in C: Gated DeltaNet, RWKV-7 core, SSD (items 3 and 4) (2026-09-12)

Gate: `make cnet_vsa_rlm_bench` (CNET_VSA_RLM_BENCH_PASS, T1 hermetic, 5 s): finite-difference gradient check of every mixer
in double precision with a carried state, then a learning check below the unigram entropy with seed-exact repeatability.
Tools: `make cnet_vsa_rlm` (train / eval / generate / score, OpenMP) and `make cnet_vsa_rlm_recall` (synthetic
associative recall). Nothing here touches the router, the capsules, the HDC/VSA memory or the n-gram generator; this is
an independent module (`include/cnet_vsa_rlm.h`, `src/cnet_vsa_rlm.c`, 520 lines) that trains from scratch on the
retained corpora.

## What was built

One trainer, three interchangeable token mixers, each a matrix-state recurrence per head (state dh x dh, read o = S q):

| mixer | state update | extra per-token parameters |
|---|---|---|
| `deltanet` (Gated DeltaNet) | S <- alpha S (I - beta k k^T) + beta v k^T, k unit | scalar decay alpha = exp(-softplus(.)), write rate beta = sigmoid(.), short causal conv (4 taps) on the mixer input |
| `rwkv7` (RWKV-7 core) | S <- S (diag(w) - kappa (a o kappa)^T) + v k^T | vector decay w, vector in-context rate a, unit removal key kappa, token shift mu |
| `ssd` (Mamba-2 SSD form; the Mamba-3 entry point) | S <- exp(-Delta A) S + (Delta x) k^T, o += D o x | per-head Delta = softplus(.), log A, skip D, short conv |

Around each mixer: RMSNorm, silu-gated output projection, residual, RMSNorm, silu MLP, residual; tied embedding and output
matrix; truncated backpropagation through time over chunks with the recurrent state (matrix state, token-shift and conv
history) carried across chunks. The backward pass is written by hand: the token loop propagates the gradient through the
recurrence (dS' = dS D^T for RWKV-7 in O(dh^2) via the rank-one structure of D, the delta-rule chain for DeltaNet, the
key-normalisation chain, the deferred token-shift and conv gradients), caches every linear map's output gradient, and the
matrix gradients are accumulated once per chunk as rank-n updates (one pass over each matrix per chunk instead of one per
token; the output layer is a chunk-level GEMM). That restructure took the single-thread cost from 4.6 ms to 0.57 ms per
token (forward + backward, d = 256, 2 layers) with the gradient check unchanged at 1e-9. Data parallelism: workers share
the parameters by pointer, own their state and gradient, reduce into the master; Adam with decoupled weight decay,
global-norm clipping, warmup + cosine schedule.

Mamba-3: its exact discretisation is not in this tree and is not claimed. `ssd` is the state-space family entry
(same state form, scalar decay per head); the paper's update is the place to swap in when it is verified.

Gradient check (d 16, 2 heads, 2 layers, vocab 23, chunk 7, state carried from a previous chunk, double precision,
random unit direction over all parameters, central differences eps 1e-5): relative error 9.4e-10 (deltanet),
8.3e-10 (rwkv7), 3.2e-9 (ssd). Learning check (2-state Markov stream, vocab 8, 300 chunks of 32): all three reach
1.383 nats against a unigram entropy of 1.762 and an oracle of 1.414; two runs with one seed agree to the last digit.

## Benchmark: language modelling on the retained corpora

Data: the 1068 training corpora of the frozen arena (`var/arena_cache/cnet/train`, 1,153,537 word-level tokens after
lowercasing, punctuation as tokens, `<eos>` per corpus); vocabulary the 8190 most frequent types plus `<unk>` and
`<eos>` (97.3% token coverage, 2.7% unk). Held-out: the 2136 test sentences of `corpora.tsv` (two tail sentences per
corpus, never in the training text), 37,702 tokens, 3.1% unk, scored per corpus from a fresh state. Perplexity is
word-level over the 8192 vocabulary, so it is comparable across the rows of this table and not to subword models.

Baseline: interpolated trigram with absolute discounting (D 0.75) on the same tokens: **held-out perplexity 146.8**.

Models: d 256, 4 heads (dh 64), 2 layers, MLP 512, chunk 64, 5 streams, Adam lr 2e-3 (warmup 200, cosine to 10%),
clip 1.0, seed 1, three runs concurrently on the 16-core host (5 threads each).

Round 1 (8 epochs, weight decay 0.01):

| epoch | deltanet (3.42M params) | rwkv7 (3.67M) | ssd (3.41M) |
|---|---|---|---|
| 1 | 126.5 | 140.4 | 126.3 |
| 2 | 96.0 | 112.0 | 96.7 |
| 3 | 85.6 | **103.9** | 86.2 |
| 4 | 82.4 | 104.2 | 82.5 |
| 5 | **81.7** | 109.8 | **82.0** |
| 6 | 81.9 | 119.0 | 84.0 |
| 7 | 83.6 | 129.9 | 86.2 |
| 8 (final) | 87.2 | 140.0 | 89.4 |

Every mixer beats the trigram from epoch 1 on; the two conv-input mixers reach 82 (44% below the trigram) and the
RWKV-7 core 104. All three then overfit (train loss 2.8 nats at epoch 8 against a held-out 4.4): 1.15M tokens is small
for 3.4M parameters, and weight decay 0.01 does not hold it. Round 2 below repeats the runs with weight decay 0.1 over 6
epochs and keeps the best held-out checkpoint (`model_best.rlm`).

Round 2 (6 epochs, weight decay 0.1, best epoch kept):

| epoch | deltanet | rwkv7 | ssd |
|---|---|---|---|
| 1 | 137.7 | 153.3 | 133.6 |
| 2 | 107.3 | 114.7 | 102.5 |
| 3 | 91.0 | 97.6 | 87.3 |
| 4 | 80.7 | 90.3 | 79.4 |
| 5 | **75.1** | **88.3** | **75.5** |
| 6 | 75.3 | 91.2 | 75.5 |

Weight decay 0.1 is worth 7 to 16 points of held-out perplexity at the best epoch (81.7 -> 75.1 deltanet, 103.9 -> 88.3
rwkv7, 82.0 -> 75.5 ssd) and flattens the overfitting tail. Ranking on this corpus: deltanet and ssd level at 75
(49% below the trigram), rwkv7 at 88 (40% below); the RWKV-7 core has 12% more parameters (its per-channel decay,
removal and rate maps are d x d each) and the vector-decay state is harder to fit at this size. All three curves are
still falling at epoch 5 under the cosine schedule, so a longer schedule with the same decay would move every number;
the comparison is at equal budget, not at convergence.

Throughput (3 runs sharing 16 cores, 5 streams each): 3.5-4.5k tokens/s per run (train + backward), about 5 minutes per
epoch. One run alone with 16 streams: 13.4k / 12.2k / 13.6k tokens/s (deltanet / rwkv7 / ssd), 85-95 s per epoch of
1.15M tokens. Forward only (ssd, best checkpoint): held-out evaluation 5.5k tokens/s on one core (9k on 8), sampling
3.7k tokens/s on one core (0.27 ms per token); resident memory 38 MB for evaluation; model file 13-14 MB.

## Benchmark: associative recall with re-binding (memory writing and erasure)

`bin/cnet_vsa_rlm_recall`: a sequence holds P key/value pairs, then R re-bindings of already used keys to new values,
then Q queries `? k v`; the loss and accuracy are on the value after each query and the target is the LATEST value of
the key (so the model must overwrite, not just store). d 64, 2 heads, 2 layers, 8000 steps x batch 8 (64k sequences),
lr 3e-3, evaluation on 200 fresh sequences.

| task (keys / pairs / re-bindings / queries) | chance | deltanet | rwkv7 | ssd |
|---|---|---|---|---|
| 16 / 8 / 4 / 8, d 64 | 6.3% | 22.8% (re-bound 26.4%) | **99.6% (99.9%)** | 31.1% (36.6%) |
| 16 / 8 / 4 / 8, d 128 | 6.3% | 21.2% (22.9%) | **99.9% (100%)** | 10.0% (10.0%) |
| 32 / 16 / 8 / 16, d 64 | 3.1% | 4.4% | 10.2% | 15.6% |
| 32 / 32 / 16 / 16, d 64 | 3.1% | 3.0% | 4.2% | 3.0% |

Sensitivity sweep on the 8-pair task (conv taps initialised on the current and previous token instead of uniform;
lr 1e-2): deltanet 26.5% / 11.8% / 6.5%, ssd 23.0% / 12.0% / 57.4%, rwkv7 at lr 1e-2 22.5%. Reading: within this
budget the RWKV-7 core (token shift + vector decay + in-context removal) learns exact recall and exact overwriting; the
two conv-input mixers as implemented here do not, at any setting tried, and the 16-pair task is out of reach for all
three at this size. This is a statement about these implementations at 64k training sequences, not about the
architectures' capacity in the literature; the delta rule's recall advantage reported there was not reproduced here.

## Quality: generation on the 71 in-domain prompts (same protocol as the coherence record)

Same 71 capsules and prompts (the capsule's own first frozen teacher question), 60 tokens, seed 7, temperature 0.8,
top-k 40, `<unk>` masked; the same measures as `result/cnet_vsa_generation_coherence_20260912.md`: perplexity under
gemma4 (Q4, ctx 256, llama-perplexity), Mistral-Small-3.2-24B absolute coherence 1-5 and "answers the question",
pairwise coherence against the n-gram capsule generation of that record (both A/B orders must agree), and the
copied-span structure (share of output 4-grams found verbatim in the routed capsule's corpus, and in the whole training
text). Two modes per model: `plain` (the prompt alone from a fresh state) and `primed` (the routed capsule's own corpus,
last 512 tokens, fed into the recurrent state before the prompt: the mixture-of-memories use, capsule supplies the
context, the shared model supplies the language). Judge n < 71 where the model closed the text within 20 characters.

Round-2 best checkpoints:

| text | gemma ppl | coherence 1-5 (>= 4) | answers | pairwise vs n-gram: win / lose / tie | 4-grams in capsule / in training text | sentence ends per output |
|---|---|---|---|---|---|---|
| extractive passage (record) | **87** | 3.97 (97%) | **85%** | preferred 99% | 100% / 100% | |
| n-gram capsule generation (record) | 882 | 3.18 (18%) | 0% | | 63% / | 0 |
| deltanet plain | 783 | 3.04 (4%) | 0% | 6 / 16 / 48 | 0% / 25% | 3.0 |
| deltanet primed | 692 | 3.01 (1%) | 0% | 10 / 14 / 46 | 0% / 26% | 2.7 |
| rwkv7 plain | 427 | 3.03 (3%) | 0% | 10 / 10 / 47 | 1% / 28% | 3.5 |
| rwkv7 primed | **369** | 3.07 (7%) | 0% | 10 / 10 / 47 | 1% / 29% | 3.3 |
| ssd plain | 539 | 3.07 (7%) | 0% | 15 / 10 / 44 | 0% / 25% | 3.4 |
| ssd primed | 646 | 2.99 (0%) | 0% | 9 / 17 / 45 | 0% / 21% | 3.1 |

Round-1 final (overfit) checkpoints, for the record: deltanet 841 / 606 (plain / primed gemma ppl), pairwise 6-17-45 /
8-14-47; rwkv7 703 / 811, 14-13-41 / 11-15-44; ssd 451 / 1076, 13-12-46 / 6-25-40; coherence 2.9-3.1, answers 0-1%.

Example (heat_exchanger_design, "how do thick fluids affect pressure drop and what tube designs help with them?"):

- ssd plain: ". the combination of these forces defines the transition from heterogeneous soil structure to soil matrix
  structure. these parameters serve as an infeasible solution of shear strength. the method of three-dimensional
  heterogeneous soils relies on mathematical"
- rwkv7 primed: ". the critical temperature method for thermal shock is inversely proportional to the material's
  composition and the thermal conductivity of the air material. the high emissivity of the medium dictates the maximum
  velocity and stability of the fluid fluids."
- n-gram (record): "High viscosity fluids increase pressure drop and manufacturing constraints tube diameter is the tube
  bundle diameter for proper fluid distribution uniformity and the tube wall thickness ..."

Reading:

- **Sentences, no topic.** The recurrent models write sentence-shaped technical prose (3 sentence ends per 55 words,
  gemma perplexity 370-780 against 882 for the n-gram walk and 87 for a real passage) that is not about the capsule:
  0-1% of their 4-grams come from the routed capsule's corpus (the n-gram walk: 63%), a quarter from the training text
  at large. They answer 0 of 71 prompts, the same as the n-gram walk; the extractive passage answers 85%.
- **The judge cannot tell them apart.** Absolute coherence sits at 3.0 for every model, mode and round (the record noted
  the judge gives shuffled words a 3); pairwise against the n-gram walk the results are ties in two thirds of the pairs
  and the wins and losses inside noise (ssd plain 15-10, deltanet plain 6-16, rwkv7 10-10). Local fluency (perplexity)
  and judged coherence are not the same thing here: the n-gram walk's copied seven-word runs and the recurrent models'
  fluent but unanchored sentences are both "partly readable with broken transitions".
- **Priming with the capsule corpus does not anchor it.** Feeding the capsule's own 512 tokens into the state before
  the prompt changed gemma perplexity both ways (rwkv7 427 -> 369, ssd 539 -> 646) and left the capsule 4-gram share at
  0-1%. A 3M-parameter state does not carry 500 tokens of specific content forward as retrievable phrases; what it
  carries is register. This is the mixture-of-memories reading in practice: the capsule memory has to supply the text
  (as the answer path does), the shared language model cannot recover it from a primed state at this size.
- Held-out perplexity does not predict judged generation across mixers: deltanet has the best held-out perplexity
  (75.1) and the worst gemma perplexity and pairwise result on the samples; rwkv7 the reverse. With 71 prompts and
  sampled outputs this is within noise, and neither number moves the answer rate off zero.

## The RLM as a coherence scorer

The one use that measured well. Scoring the three texts of each of the 71 coherence-record items with the recurrent
model's own per-token loss (`cnet_vsa_rlm score-batch`, fresh state, `<eos>` context), the models reproduce gemma4's
ordering (extract 87 < n-gram generated 882 < words shuffled 19,614) item by item:

| model (round-2 best) | median perplexity: extract / n-gram generated / shuffled | items with extract < generated < shuffled |
|---|---|---|
| deltanet | 29 / 135 / 11,678 | 68 / 71 |
| rwkv7 | 26 / 225 / 15,096 | 70 / 71 |
| ssd | 26 / 123 / 14,080 | 69 / 71 |

Round-1 finals give the same picture (69, 71, 69 of 71; medians 17-21 / 159-263 / 45k-64k). A 13 MB C model that
scores 60 tokens in about 12 ms on one core separates a real passage from a phrase mixer from word salad as reliably
as the 26B judge does, without a GPU. That is a usable component: a fluency floor for anything the generator or the
answer path returns, and a tie-breaker between candidate passages, both in C and both hermetic.

## Scorer wired into the answer path: measured, and rejected as an answer-path component

Wiring (kept, off by default): the registry takes an optional text scorer hook (`cnet_vsa_registry_set_scorer`,
fields `scorer_fn/ctx/rerank/floor`) that acts only AFTER the calibrated VSA floor accepted an answer: `rerank`
reorders the accepted top-k by pmi = nll(passage) - nll(passage | prompt) (the accepted set and the certification are
unchanged), `floor` refuses an accepted answer whose top passage scores above a nll threshold. The recurrent model
plugs in through `src/cnet_vsa_rlm_score.c` (tokenizer + vocabulary shared with the trainer, `cnet_vsa_rlm_scorer_nll`);
the CLI installs it from `CNET_VSA_RLM_MODEL=<dir>` with `CNET_VSA_RLM_RERANK=1` / `CNET_VSA_RLM_FLOOR=<nll>`, prints
an `LM:` line per answer, and `passages-dump` lists every certified passage. Gate: `make cnet_vsa_answer_bench` check
4/4 trains a 13k-parameter model in-test, installs it, and asserts that the rerank only permutes the accepted set
(status, capsule and set identical to the baseline) and that the floor only removes (above every score: unchanged;
below every score: every accepted answer becomes REFUSE_FLUENCY, refusals untouched).

Measured on the production registry (812 capsules), the 400 frozen teacher questions of the answer-path record, the
same Mistral judge on the returned top passage, scorer = the round-2 SSD model:

| answer path | answered | judged correct of answered | correct of all 400 | wrong of all 400 | top passage changed | scoring cost per answer |
|---|---|---|---|---|---|---|
| baseline (VSA order, today's judge run) | 164 (41%) | **77%** (126) | 32% | 10% | | 0 |
| + pmi rerank over 4 accepted candidates | 164 | 41% (68) | 17% | 24% | 120 / 164 | 42 ms |
| + pmi rerank over 2 accepted candidates | 164 | 57% (93) | 23% | 18% | 80 / 164 | 21 ms |

The rerank is actively harmful: the conditional likelihood of this model, fed a question it never saw the like of in
training, moves the answer away from the passage the question is about. In the smoke test on "wavefront lds shared
memory coalescing hip execution" the passage that answers it ("LDS is a programmable shared memory space ...") had
the most NEGATIVE pmi of the four (-0.39): the question prefix makes the relevant passage less likely, not more. The
judge confirms it on 164 answers: precision 77% -> 41%. Rejected; rerank stays off.

Fluency floor: nll of all 47,774 certified passages (median 3.69, p90 4.68, p95 4.99, p98 5.37, p99 5.65, max 7.91).
The worst-scored passages are exactly the teacher truncations the lexicon record noted ("No real heat engine can
exceed.", "The significance lies.", "Inadequate fluidization."; 129 passages of six tokens or fewer), so the scorer
does identify broken passages. But they never reach the top of an answer: on the 400 questions a floor at p98 removes
0 of the 164 judged answers (2 in the live run), at p95 it removes 3 to 5, all of them judged correct; judged-correct
and judged-wrong answers have the same median nll (3.60 vs 3.65). The VSA ranking already keeps fragments out of the
top slot (a five-token fragment carries little topical weight), so the floor is a no-op on the answer path at 7 ms
per answer. Rejected as an answer-path gate; floor stays off. The one place the finding applies is sealing-time
hygiene (drop passages a fluency scorer flags before they are certified), which would touch the capsule format's
receipt and was not done here.

Cost of the wiring when off: none (a NULL hook). When on: one forward pass per scored passage, 0.33 ms per token
single-threaded in the CLI build, i.e. about 7 ms for a floor check and 20 to 40 ms for a rerank, against 60 us for
the route and rank.

## Verify ladder and footprint

- T1: `cnet_vsa_rlm_bench` (double-precision gradient check + learning check), ledger row in `tests/verify_logs.sh`;
  `cnet_vsa_answer_bench` gained the scorer-hook check (4/4) and now links the two RLM sources;
  the CORE ledger band in `tests/verify_tier_sync.sh` was widened from [20,35] to [20,40] to admit the row (a size band,
  not a certification floor; T1 already used [20,40]). Root Makefile unchanged (the fragment is `mk/vsa_rlm.mk`,
  included from `mk/vsa.mk`); budget 8494 holds.
- Not in the ladder: `cnet_vsa_rlm` (trainer) and `cnet_vsa_rlm_recall` (benchmark); model files under `var/rlm*`
  are not tracked.
- Binary footprint: the module adds no dependency (C11, OpenMP optional); the trainer is 155 KB.

## Conclusion for items 3 and 4

- **Built, in C, gradient-exact, tiny.** All three mixers train from scratch on the CPU with one 520-line module; no
  Python, no GPU, no new dependency. A 3.4M-parameter model trains an epoch of the retained corpora in about a minute
  on the 16-core host, evaluates at thousands of tokens per second on one core, and lives in a 13 MB file.
- **As language models they work and are level with each other.** Held-out perplexity 75 (deltanet, ssd) and 88
  (rwkv7) against 147 for a trigram on the same tokens; the two conv-input mixers are within a point of each other,
  which is the expected result at this scale: the state update differs, the data does not have enough long-range
  structure at 1.15M tokens to separate delta-rule erasure from exponential decay. Mamba-3's discretisation remains
  unverified in-tree and is not claimed; `ssd` is the slot for it.
- **As generators they do not improve what we have.** Fluent sentences, no anchoring to the capsule, 0% answers:
  exactly the failure the coherence record predicted for "fluent domain prose", and priming the state with the
  capsule's own text does not fix it at this size. The router's answer path (passages under a certified floor, 76%
  judged precision) stays the only answer source, as decided; nothing here replaces it or was removed for it.
- **As a scorer they reproduce the judge's fluency ordering, and that does not help the answer path.** The
  recurrent model's perplexity reproduces the 26B judge's fluency ordering on 68-71 of 71 items in C on one core.
  Wired into the answer path and measured (section above): a pmi rerank cuts judged precision from 77% to 41-57%,
  a fluency floor removes nothing the VSA ranking had not already kept out of the top slot. Both stay off; the hook
  stays in the tree for other scorers, and the fluency finding belongs to sealing-time hygiene, not answering.
- **On memory writing and erasure, RWKV-7's core is the one that demonstrably does it.** On the synthetic
  recall-with-rebinding task the RWKV-7 mixer stores and overwrites exactly (99.6-99.9%) where the DeltaNet and SSD
  implementations here stay near chance at the same budget. The literature's delta-rule recall advantage was not
  reproduced; whether that is the conv-input path, the scalar gates or the 64k-sequence budget is open, and the
  benchmark tool is in the tree to settle it.

WITHHELD: any claim that these models answer questions; any Mamba-3 claim; any claim beyond the measured 64k-sequence
recall budget. Measured and rejected: the scorer as an answer-path rerank or floor.
