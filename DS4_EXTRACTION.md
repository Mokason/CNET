# Extracting from DwarfStar (ds4) into CNET

DwarfStar (`/home/marble/AI/ds4`) is an 80k-line C inference engine for
DeepSeek V4 — same language, same "correctness before speed, no unexplained
drift" ethos as CNET. Its teacher-oracle path is precisely the job CNET's
`cce_gguf.c` does, done world-class. This maps its reusable subsystems onto
CNET's known wounds, ranked by value/risk. (Survey: 2026-07-08.)

## Adopted

- **Dequant cross-check** (`tests/test_dequant_xcheck.c`, done) — CNET's
  Q8_0/Q4_K dequant vs ds4's independent GGML-lineage arithmetic, bit-checked
  over 8192 blocks. The regression guard the Q4_K placeholder never had.
  Wired into `make verify`.

## Tier 1 — Loader memory model (cures the 140 GB/instance wall)

ds4 **never bulk-dequantizes**: one `mmap` (`ds4.c:1970`), parse only the
tensor directory, and matmul kernels consume GGUF-quantized weights directly
while quantizing the *activation* per call (`matmul_q8_0_batch` `ds4.c:5223`;
IQ2 dot `ds4.c:2971`). The 2-bit file IS the in-memory representation. CNET's
`cce_gguf_load_f32` does the opposite — dequants every tensor to fp32 (~140 GB
for a 7 GB Q4 file). Adopt: keep weights in-format, add quantized-weight ×
quantized-activation matmul to the oracle forward. Biggest structural win;
biggest surgery (the whole forward assumes fp32 tensors). Start with the
largest tensors. NOTE CNET already has `CNET_ORACLE_INT8` (int8 branches) as a
partial version of this idea.

## Tier 2 — GGML-free quant library (`gguf-tools/quants.[ch]`)

Self-contained Q8_0/Q4_K/Q2_K/IQ2_XXS quant+dequant with imatrix weighting and
f16/bf16 conversion, no GGML link (`quants.c` 1109 lines). Lift wholesale to
give CNET correct, tested coverage + the FP8-E4M3/FP4-E2M1 input dequant
(`deepseek4-quantize.c:638,711`). Low risk (standalone C).

## Tier 3 — ROCm backend (replaces OpenCL `cce_clgemm`)

`ds4_rocm.h` is a drop-in CUDA→HIP macro shim: one CUDA `.cu` kernel set
compiles under both nvcc and hipcc with zero `#ifdef` in the kernels, incl.
portable `__dp4a`→`amd_mixed_dot` (`ds4_rocm.h:113`). Adds WMMA (wave32 RDNA
builtins — **gfx1201/R9700 is the right RDNA4 family**), hipBLASLt plan cache
(`ds4_rocm_hipblaslt.cuh:38`), quantized matmul. Build for this box with
`ROCM_ARCH=gfx1201` (default is gfx1151; tile sizes tuned for gfx1151 —
`ds4_rocm_matmul.cuh:672`). Large integration; CNET's clgemm works today, so
medium-term.

## Tier 4 — Validation harness upgrade

ds4's official-vector harness (`tests/ds4_test.c:846`) asserts per step:
argmax token byte-equality AND every official top-20 logprob present locally
within ±4.0, PLUS a separate `local-golden.vec` that catches backend drift
keeping the same argmax but damaging the distribution. CNET's golden battery
is 32 argmax probes — add the distribution check. Cheap, high-integrity, on
the exact seam CNET already has. (Needs reference logprobs per teacher.)

## Tier 5 — C tokenizer + generation stack

Full byte-level BPE in C (`bpe_tokenize_text` `ds4.c:22153`, `vocab_load` from
GGUF `ds4.c:22233`) + sampler chain (temp→top-k→min-p→top-p→xorshift
`ds4.c:22706`) + chat template. CNET mines in token-id space (no tokenize
needed) and serves via the .NET Gemma tokenizer, so this is only needed if the
C side ever goes text→units. Deprioritized.

## Tier 6 — Byte-prefix disk KV checkpoints (`ds4_kvstore.c`)

Session state keyed by SHA1 of rendered byte-prefix, hit-decayed eviction
(`ds4_kvstore.c:532`). For CNET: persist mining prefixes ([BOS,t]) across runs
and lanes so a resumed campaign doesn't recompute them. Modest win given
CNET's prefixes are 1-2 tokens; revisit if pair/longer-context mining lands.

## Not applicable

MLA compressed-KV, hyper-connections + Sinkhorn, MoE expert streaming/hotlist,
MTP speculative decode, distributed pipeline — all DeepSeek-architecture or
chat-serving specific; CNET's oracle is a dense forward for argmax/topk, not a
long chat generator.
