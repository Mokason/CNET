# Recurrent memory architectures against the capsule router: what the math says (2026-09-12)

Question: can Gated DeltaNet(-2), Mixture-of-Memories, RWKV-7 and Mamba-3 be added, in C, to improve the
certified capsule system without removing anything (HDC encoders, calibrated gates, lexicon, passages)?

## What we have, written as memory equations

- Capsule topical memory: a bundle, c = normalize(sum_i x_i) over the sealed sentences, int8-2048. Routing is
  nearest centroid under calibrated radii and margin/ambiguity/term gates. This is a Hebbian associative memory
  with one slot per capsule; interference between capsules is what the gates measure and refuse.
- Capsule generative memory (`cnet_vsa_ngram.c`): an explicit table of (context key, next token) pairs with
  context k_t = P^2(w_{t-1}) (x) P^1(w_t) (permute-and-bind), read by scanning every stored key (cosine > 0.2),
  plus a bundled global binding M = sum_t v_t (x) k_t read by unbinding M (x) q. The bundle is the vector-valued
  Hebbian memory: recall of v_j from key k_j carries crosstalk sum_{i != j} v_i (x) k_i (x) k_j. Its file cost is
  the whole reason a capsule is 5.6 MB.
- Registry: K independent memories, one selected per query by a calibrated router; abstain when none certifies.

## The four architectures, reduced to their state update

All four keep a matrix state S (per head, d x d, or diagonal in the SSM case) and differ in the transition:

| model | update per token | what it adds over a Hebbian sum |
|---|---|---|
| DeltaNet / Gated DeltaNet | S <- alpha_t S (I - beta_t k_t k_t^T) + beta_t v_t k_t^T ; read S q | the delta rule: ERASE the old value at key k before writing the new one (a rank-1 Householder-like step); alpha_t is a scalar forget gate. For linearly independent keys this is exact recall of up to d pairs with no crosstalk; with alpha = 1 it is a recursive least-squares memory without forgetting. "DeltaNet-2", if it means a 2026 successor, I do not have its equations; the delta-rule reading holds for the family (DeltaNet, Gated DeltaNet, DeltaProduct = several Householder steps per token). |
| Mixture-of-Memories (MoM) | K states S^(k); a router sends each token to its top-k memories (sparse activation), each updated as above; a shared memory always updated; outputs mixed | memory partitioning to cut interference, learned end to end. It is our registry with a learned router and a shared memory, minus certification. |
| RWKV-7 | S <- S (diag(w_t) - kappa_t^T (a_t o kappa_t)) + v_t^T k_t ; read S r_t, with token shift and a channel MLP | a generalized delta rule: a VECTOR of in-context learning rates a_t (per-key erase strength) and a vector decay w_t, which is what lets it track state that plain linear attention cannot. |
| Mamba-2 (SSD) / Mamba-3 | S <- alpha_t S + v_t k_t^T with scalar (Mamba-2) or richer discretised decay; no erase term | selective decay, cheap (diagonal/scalar), no delta erasure. The tree already runs Mamba-1 inference (`src/cce/cce_ssm.c`) and hybrid attention+SSM inference (`cce_hybrid.c`). I cannot vouch for Mamba-3's exact update from memory; treat it as the SSM family member to test, and give me the paper for the precise discretisation before any implementation. |

Per token, per head of width d: DeltaNet and RWKV-7 cost about 3 d^2 multiply-adds (read, erase, write); SSD costs
d x n with a small n. At d = 64, 8 heads, 4 layers that is well under a million operations per token: trivially
C, no GPU needed for inference. Training needs backward through the recurrence; the in-tree C/HIP trainer
(`src/cce/cce_moe_xf.c`: one transformer block, CPU/GPU GEMM dispatch, Adam, finite-difference gradient-checked)
is the harness a recurrent mixer would slot into in place of the attention term.

## Where each one maps onto our system, and what it would measurably change

1. **The delta rule as the capsule's generative memory (from DeltaNet).** Replace the bundled global binding and
   the scanned transition table with one matrix memory S (d x d) written by the delta rule from the capsule's
   own sentences: k_t the permute-bound context, v_t the next-word vector. Read is one matvec (d = 512: 262k
   MACs, about 25 us) instead of a scan over every transition. The delta rule gives exact recall for up to d
   stored contexts and least-squares recall beyond; the Hebbian bundle degrades with every added transition.
   Because v4 capsules carry their sentences, S need not be stored at all: it is derived at load or on first
   use from the certified passages (about 20 ms per capsule), so the memory is certified by derivation and the
   file does not grow. Nothing is deleted: the explicit table stays as the reference and the gate compares them.
   Measure: next-word recall on the capsule's own sentences and on held-out sentences; generation perplexity
   under the same gemma protocol as the coherence record; file size and load time. Expected: recall up
   strongly, local coherence slightly up, GLOBAL coherence unchanged, because a d x d memory of bigram contexts
   is still not a language model. Effort: a few hundred lines of C, one gate, days.

2. **MoM's sparse mixing applied to answering.** Our router already is a mixture of memories with top-1 sparse
   activation and abstention. MoM's two ideas we do not have: top-k activation with mixing, and a shared memory.
   The first is directly testable: when the ambiguity gate refuses between two siblings (it is the largest
   refusal class), rank the passages of BOTH capsules and answer if the best clears its floor. Expected: part of
   the 24% ambiguity refusals at k = 1 become answers at the passage precision we measured (about 76%). A shared
   memory is the lexicon-level fallback we do not want (an uncertified answer source); skip it. Effort: hours,
   measured with the existing judge protocol.

3. **RWKV-7 / Gated DeltaNet as a small recurrent LM trained from scratch, in C/HIP.** This is the only route to
   the "fully recurrent language generation" baseline the records have withheld since day one. Data: the 770k
   content tokens of the technical corpora (plus 1.4M with Isekai), word-level vocabulary from the lexicon
   (11.5k) or the in-tree tokenizer. Model: 4 layers, d = 256, about 15 to 20M parameters. Training in C/HIP
   through the existing block trainer with the attention term swapped for the delta-rule mixer (implement the
   core once; RWKV-7 is the same recurrence with vector w and a); one epoch of 770k tokens is minutes on the
   R9700s once the trainer is not overhead-bound (it measured about 500 tok/s on the MoE block: that is the
   first thing to fix, or the run is hours). What it is for: (a) a coherence/answer SCORER: perplexity of a
   candidate passage given the question, to rerank the top passages and to score the n-gram generator, which
   the judge protocol can validate; (b) the generation baseline: expect fluent domain prose that answers some
   questions and invents others. What it is not: an answer path. Its output cannot be certified, so it never
   replaces the passage path; it sits beside it as a scorer and a measured baseline. Effort: weeks (trainer
   throughput, recurrent backward, tokenizer, gates).

4. **Mamba-3 against DeltaNet.** Same harness, second mixer. At our data scale (under 2M tokens) the two
   families will be separated by the delta erase term, not by discretisation details, and a memory task (recall
   of the capsule's own sentences) is the one place the difference is predictable: the delta rule wins on
   exact recall, the SSM wins on cost. Worth running only after 3 exists, and only with the paper's equations
   in hand.

## Conclusion

It is all the same equation with different erase terms. Our capsules are Hebbian memories with certified gates
around them; the four architectures are learned delta-rule memories with no gates. The transferable piece is
the erase term, and it applies where we have a memory problem, not a routing problem: the generative memory
inside a capsule (item 1) and the way siblings are combined at answer time (item 2). Both are small C changes,
keep everything that exists, and have gates ready to measure them. The recurrent LMs (items 3 and 4) are a real
project: they give the missing generation baseline and a useful scorer, they do not give certified answers, and
they should be built on the C/HIP trainer only after its throughput is fixed. Order: 1, 2, then 3 with Gated
DeltaNet first and RWKV-7 sharing its core, then 4.

Not claimed: any number for items 1 to 4; those come from the gates when they run.

## Outcome 2026-09-12 (items 3 and 4 built and measured)

Built as `src/cnet_vsa_rlm.c` (one trainer, three mixers: Gated DeltaNet with short conv, RWKV-7 core, SSD as the
state-space slot; explicit backward, double-precision gradient check in `make cnet_vsa_rlm_bench`; CPU + OpenMP, not
the HIP block trainer: the chunk-level rank-n gradient restructure made the CPU path 0.57 ms per token and the corpus
trains in a minute per epoch, so the GPU trainer was not needed). Measured in `result/cnet_vsa_recurrent_lm_20260912.md`:
held-out perplexity 75 / 88 / 75 (deltanet / rwkv7 / ssd) against 147 for a trigram; generation fluent, unanchored,
0% answers, ties the n-gram walk under the judge; the model as a coherence scorer reproduces the 26B judge's ordering
on 68-71 of 71 items; on recall-with-rebinding RWKV-7 stores and overwrites exactly, the other two do not at the
tried budget. Mamba-3's discretisation: still not in the tree, not claimed. Predictions above that held: the LMs are a
scorer and a baseline, not an answer path. Prediction that did not hold: the delta rule winning exact recall at this
scale (not reproduced; open). Scorer wired into the answer path (optional hook, gate check in the answer bench) and
measured on the 400 judged questions: pmi rerank 77% -> 41-57% precision, fluency floor a no-op; rejected, off by
default. The scorer's real finding is passage hygiene at sealing time (it flags the teacher truncations), not answering.
