# Integrity Slice — Q5_K and GGUF Refusal

**Place:** GGUF ingestion in `src/cce/cce_gguf.c`.

**Dilemma:** Q5_K currently uses an admitted rough decoder, and malformed KV/tensor records can be accepted partially.

**Consequence:** A model may load successfully with silently corrupted weights or metadata.

**Systematic component:** byte-exact reference blocks and deterministic parser errors. **Noise:** floating-point representation only; compare using the reference decoder’s defined tolerance.

## TDD

1. Add CPU-only Q5_K fixtures generated from the llama.cpp block layout; watch current decode disagree.
2. Implement the exact block layout and per-sub-block scale/min unpacking.
3. Add truncated/malformed KV and tensor-table fixtures; watch current loader accept them.
4. Make metadata parsing fail closed and release partial allocations.
5. Focused marker: `GGUF_INTEGRITY_PASS`.
6. Parent wires the target into `release_integrity` before another slice is accepted.
