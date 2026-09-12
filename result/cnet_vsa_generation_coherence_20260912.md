# Capsule text generation: is it coherent? (2026-09-12)

Every routing record so far marked generated-text quality WITHHELD. This is the measurement. Nothing here is a gate;
it is a diagnostic on the shipped generator (`cnet_vsa_gencap_generate_ex`: gate on the prompt, then an n-gram walk
through the capsule's own corpus with VSA unbinding, steer weight 0.45, temperature 0.85).

## Protocol

100 sealed capsules from bin/ sampled at random among those with frozen teacher questions; the prompt is the capsule's
own first frozen question; 60 tokens; seed 7. 29 prompts were refused by the float-space gate (legacy v1/v2 capsules in
bin/ have no wide block and are gated at a fixed 0.90), 71 generated. Three texts per capsule were compared:

- the generated output;
- an extractive baseline: the corpus sentence with the most content-word overlap with the prompt, plus its successor
  (what a retriever would return instead of generating);
- the same generated output with its words shuffled (floor), and with its 7-word chunks shuffled (does the walk's
  ordering across seams carry anything beyond its copied runs?).

Measures: copied-span structure against the capsule's corpus; perplexity under gemma4 (Q4, context 256,
llama-perplexity); pairwise coherence preference by Mistral-Small-3.2-24B with both A/B orders required to agree;
absolute 1-5 coherence and "answers the question" by the same judge.

## Results

| measure | extractive passage | generated (60 tokens) | 7-word chunks shuffled | words shuffled |
|---|---|---|---|---|
| perplexity under gemma4 | **87** | 882 | 1,844 | 19,614 |
| Mistral: coherence 1-5 (mean; share >= 4) | 3.97 (97%) | 3.18 (18%) | | 3.01 (3%) |
| Mistral: answers the question | **85%** | **0%** | | 0% |
| pairwise vs generated (both orders agree) | extract preferred 99% | | generated preferred 55%, tie 45% | generated preferred 89%, tie 11% |

Copied-span structure of the generated text (71 outputs, per 60 tokens): 63% of output 4-grams occur verbatim in the
corpus; copied runs are 7.4 words long on average (longest run 13.5); 8 seams per output where one copied run ends and
an unrelated one begins; distinct-bigram ratio 0.97 (no looping); zero sentence boundaries emitted (the period is
appended by the caller).

Example (heat_exchanger_design, prompt "how do thick fluids affect pressure drop and what tube designs help with them?"):

- generated: "High viscosity fluids increase pressure drop and manufacturing constraints tube diameter is the tube bundle
  diameter for proper fluid distribution uniformity and the tube wall thickness is insufficient relative to the shell
  diameter and tube configurations drop across the tube layout pattern such as finned tubes or corrugated pl"
- extractive: "High viscosity fluids increase pressure drop and require larger tube diameters or specialized geometries
  to maintain flow. Low thermal conductivity necessitates increased surface area or enhanced surf"

## Reading

- **Locally coherent, globally not.** The output is on topic and made of real corpus phrases about seven words long,
  which is why it beats word salad 89:0 and its perplexity is 22x better than shuffled words. Every seven words it
  jumps to an unrelated phrase, never closes a sentence, and its perplexity is 10x worse than the passages it copies
  from. The walk's ordering across seams carries a little (2x better than shuffling its own chunks, preferred 55:0
  with 45% ties), not sentence structure.
- **It never answers.** 0 of 71 outputs were judged to answer the prompt, against 85% for the extractive passage from
  the same corpus. The judge's absolute scale is weak (it gives shuffled words a 3), so the pairwise and perplexity
  numbers are the ones to trust; the answer rate is unambiguous either way.
- **The router's value is in what it retrieves, not what it generates.** The same capsule holds sentences that answer
  its own questions 85% of the time when returned verbatim. The compact retriever measured separately
  (result/cnet_vsa_retrieval_20260912.md) is the path that turns routing into answers; generation as it stands is a
  topic-conditioned phrase mixer and should not be presented as an answer path.
- 29% of a capsule's own teacher questions are refused by the legacy float gate in bin/; resealing bin/ under the v3
  block and lexicon (the rollout item that is still open) is a precondition for any answer path.

WITHHELD (still): generated-text factual correctness beyond "does not answer"; any generation claim against a
language model, which would be a comparison of a phrase mixer with a generator and is not made.
