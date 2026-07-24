# gigatok_bench — independent confirmation of gigatoken's core claim

[Gigatoken](https://github.com/marcelroed/gigatoken) is a Rust BPE tokenizer that
claims ~1000× over HuggingFace `tokenizers` at GB/s throughput. Its own
optimization log attributes the win primarily to **replacing the pretokenization
regex with SWAR** (SIMD-within-a-register) byte classification, plus a
dual-cursor ILP trick. This is a C port of *that lever only*, so we can measure
it ourselves on our own hardware.

Build/run: `make gigatok_bench` (generates a pseudo-OpenWebText corpus), or
`./bin/gigatok_bench <corpus_file>` to point it at real text (e.g. `owt_train.txt`).

## What it is
- `src/cce/cce_pretok_swar.c` — the GPT-2/r50k-o200k-family pretokenizer in three
  forms sharing one grammar (a macro guarantees identical output):
  - **codepoint-decode** scalar — the style `cce_gguf_tok.c` uses today;
  - **SWAR** — letter/digit/other/whitespace runs scanned 8 bytes/iteration with
    borrow-safe branchless `u64` arithmetic;
  - **SWAR + dual-cursor** — two independent scan cursors so the out-of-order
    engine fills the per-token latency bubble (the log's step 8).
- `tests/gigatok_bench.c` — gates `scalar ≡ SWAR` boundaries **byte-for-byte**,
  then times single-thread MiB/s and a multithreaded (OpenMP) aggregate GB/s.

## Measured (this repo, AMD 32-thread, AVX-512, 134 MB pseudo-OWT)
| variant | throughput | note |
|---|---:|---|
| CNET-style (codepoint decode) | ~0.70 GiB/s | today's style |
| SWAR | ~0.90 GiB/s | 1.3× |
| **SWAR + dual-cursor** | **~1.15 GiB/s** | **1.7×**, matches the log's ~1.05 GiB/s target |
| AVX-512 (single cursor) | ~0.73 GiB/s | **0.81× vs SWAR** — wider *loses* here |
| **AVX-512 + dual-cursor** | **~1.28 GiB/s** | 1.11× vs SWAR+dual |
| SWAR + dual, 32 threads | ~24 GB/s | in gigatoken's 20–24 GB/s range |
| **AVX-512 + dual, 32 threads** | **~28 GB/s** | 1.19× vs SWAR |

**Confirmed:** the single-thread SWAR+dual pretokenizer really does cross
~1 GiB/s/thread; the dual-cursor ILP really does add ~25–30%; and it scales to
~24 GB/s aggregate — i.e. gigatoken's *core throughput mechanism* reproduces
independently in C.

**Per-arch SIMD (AVX-512), and why gigatoken leans on SWAR anyway.** The AVX-512
pretokenizer (64 bytes/iteration, one compare-mask per byte class + `tzcnt`)
produces byte-for-byte identical boundaries, but the result is a *modest* and
conditional win: **single-cursor AVX-512 is slower than 8-byte SWAR (0.81×)** —
natural-text runs are short (words ~5 bytes), so a 64-byte load + full-width
compare is mostly wasted and SWAR's finer granularity ends the run sooner. Only
with dual-cursor ILP does AVX-512 pull ahead, and only by ~10% single-thread /
~19% at 32 threads (~28 vs ~24 GB/s). That matches the optimization log's own
verdict — "SWAR is the single biggest win" — and is exactly why gigatoken chose
portable SWAR over hand-tuning wide SIMD for every family: the wide path is a
small, fiddly, sometimes-negative delta on top of the ~90% SWAR already delivers.
(It also invites the all-core AVX-512 downclock; the bench measures both back to
back to keep that honest.)

## What it does NOT confirm (honest scope)
- **Not the "1000× vs HF" headline.** That needs the full stack (BPE merge +
  huge-page pretoken cache + parallel IO) and rests on an asymmetric benchmark
  (gigatoken on 11.9 GB *with* caching vs HF on 100 MB *without*), with HF
  baseline numbers that look overhead-limited. The defensible claim is
  "hundreds× over HF, GB/s absolute," which the absolute numbers above support.
- The SWAR win here is ~1.7× over an *already-fast hand-rolled scalar*; the log's
  larger multiples are against a **regex** baseline (~47 MiB/s), not a good
  scalar. Our result is consistent with the log's own SWAR-vs-scalar ratio.
- ASCII fast path: non-ASCII bytes are treated as letter-continuation (applied
  identically to both paths, so parity holds). Full `\p{L}` Unicode property
  matching and the BPE merge/cache are out of scope for this confirmation.
- The AVX-512 path speeds up *pretokenization in isolation* only. End-to-end
  encode is cache/merge-bound (see below), where pretok is ~0% — so the encode
  path keeps the portable SWAR skip; AVX-512 there would buy nothing.

## End-to-end: wired into cce_gguf_tok (`make gigatok_encode_bench`)

The SWAR pretokenizer and a **pretoken cache** are wired into the real BPE encode
path as `cce_gguf_tok_encode_fast(t, text, ids, max, flags)`
(`CCE_TOK_FAST_SWAR | CCE_TOK_FAST_CACHE`), leaving `cce_gguf_tok_encode`
untouched. The SWAR skip runs inside the *same* pretok grammar (non-ASCII and
apostrophes still go through the exact codepoint logic), and the cache memoizes
`span -> token-ids`; both are exact, and `tests/gigatok_encode_bench.c` asserts
the fast path's ids equal the baseline **byte-for-byte**.

Two more optimizations landed on the encode path, both byte-for-byte identical
(verified by an id-stream hash: old and new both give `idhash=01037b4e45cc7116`
on the deterministic Qwen/4 MB case):

1. **`bpe_word` fixed-array rewrite** — symbols are (offset,length) slices of one
   contiguous byte-encoded buffer, so a merge just extends the left slice; no
   malloc, no `strlen`, no per-symbol copies. Adjacent pair-ranks are cached and
   only the two neighbours of a merge are recomputed (vs the original full O(ns²)
   rescan). **~1.9×.**
2. **id-space merge table** — at load, `byte_to_id[256]` and a
   `(id_a,id_b) → (rank, merged_id)` table are built, so the merge loop is pure
   integer: no `fnv1a`, no `strcmp`, no key building. `merges_get`/`vocab_get`
   leave the hot loop entirely. Guarded by a load check that every byte and merge
   resolves to a valid id (clean BPE like Qwen/GPT-2); tokenizers where it
   doesn't (SentencePiece-style, e.g. gemma) safely keep the string path.
   **~2× more.**

Measured on a real GGUF vocab (Qwen-family, 248k tokens; ~4 MB corpus), all
outputs byte-for-byte identical:

| config | throughput | vs baseline | ids |
|---|---:|---:|---|
| **original** `bpe_word` (malloc/strlen) | 19 MB/s | 0.25× | identical |
| + fixed-array `bpe_word` | 37 MB/s | 0.49× | identical |
| baseline (`cce_gguf_tok_encode`, +id-space) | 75 MB/s | 1.00× | — |
| + SWAR pretok | 76 MB/s | **~1.0×** (noise) | identical |
| + pretoken cache | 280 MB/s | **3.7×** | identical |
| + SWAR + cache | 307 MB/s | **4.1×** | identical |

**The honest finding — and it matches gigatoken's own thesis.** In this encoder
(as in most) the BPE merge is the bottleneck, not pretokenization: SWAR pretok
alone buys **~0% end-to-end**. Three levers moved the *merge* itself: the
fixed-array rewrite (1.9×, killing per-symbol malloc + per-pair `strlen`), the
id-space table (~2× more, killing string hashing/compare in the loop), and the
**pretoken cache** (skips the merge entirely on the ~99% of repeated pretokens).
Note how the cache's *relative* win shrank from 14× → 3.7× as the baseline merge
got ~4× faster — caching matters less once the thing you'd cache is cheap. SWAR
only ever mattered *after* the merge cost was removed.

Combined, the encode path went from 19 → 75 MB/s baseline (~4×, no cache) and
→ 307 MB/s with cache (~16× over the original), all at verified-identical output.
The baseline (no cache) is now faster than HF `tokenizers`' single-config numbers.

The table above encodes the corpus in one call, so the per-call cache fills once
and serves the whole corpus. That is *not* how training data streams.

### Persistent cross-call cache (streaming many small documents)

`cce_gguf_tok_cache_enable(t)` allocates a cache that lives on the tokenizer and
survives across `encode_fast` calls; encode with `CCE_TOK_FAST_PERSIST` to reuse
it. Streaming the same corpus as 16k ~256-byte documents (Qwen vocab):

| streaming config | throughput | vs no-cache | ids |
|---|---:|---:|---|
| baseline (no cache) | 73 MB/s | 1.00× | — |
| per-call cache (`CCE_TOK_FAST_CACHE`) | 38 MB/s | **0.51×** | identical |
| persistent cache (`CCE_TOK_FAST_PERSIST`) | 224 MB/s | **3.1×** | identical |

The sharp result: **a per-call cache is *worse than no cache* when streaming small
documents** — it allocates and frees a table every document that barely fills, so
you pay setup with almost no reuse. The **persistent cache accumulates hits across
the whole stream** (common words are encoded once, then reused everywhere) for
~3× on Qwen and ~6.7× on gemma's slower string path. This is the shape that
matters for a data loader, and it's why gigatoken keeps its cache resident for the
whole run. (Not thread-safe: one encoder thread per tokenizer, or use the per-call
cache; a sharded/persistent cache is the concurrency extension.)

### Cache memory layout (`make gigatok_cache_bench`)

The cache is **cache-line-packed** (gigatoken's `pretoken_cache.rs` design): each
entry is exactly one 64-byte cache line with the span key AND token ids inlined,
so a hit touches **one line** — the old design followed two heap pointers (`key`,
`ids`) per hit, i.e. ~3 dependent loads. The table is 2 MiB-aligned +
`MADV_HUGEPAGE`. This only pays when the table outgrows L3 (gigatoken's ~1.3M
unique pretokens); the 50-word synthetic corpus keeps it L2-resident, so the
encode bench above can't show it. A dedicated microbench (4M entries, ~540 MB
table, 20M random hits) isolates the layout:

| layout | ns/lookup | vs pointer-chase |
|---|---:|---:|
| pointer-chase (old, key+ids via pointers) | 88.6 | 1.00× |
| packed, 4 KiB pages | 67.4 | **1.32×** |
| packed + hugepage | 67.5 | 1.31× |

**The cache-line packing is the real win (1.3×)** — inlining the key and ids
removes two dependent DRAM loads per hit. The **huge-page hint shows no
measurable benefit on this host** (THP=`madvise`, AMD box): at ~69 ns/hit
we are DRAM-latency-bound, so the page-walk/dTLB cost is largely hidden behind the
line fetch. It's kept because it's correct (madvise-before-fault ordering, faults
in as 2 MiB pages) and gigatoken measured +7–15% from it on Zen — a real
uarch/allocator-dependent effect we simply don't hit here. Honest: on this machine
the packing matters and the huge-pages don't.

### The merge-table lookup (`pair_get`) and its hash

Two things here, both instructive:

- **The hash was already fast.** `pair_get` keys on a `u64` `(id_a,id_b)` pair and
  hashes it with one multiply-shift (`key * φ⁻¹ >> 32`, Fibonacci hashing), not
  FNV. The *actual* FNV is `ptc_hash` over the pretoken cache's short byte spans.
- **A "faster" hash made it slower — measured, then reverted.** Replacing that
  FNV byte-loop with an 8-byte-at-a-time multiply-mix was **slower** for the 4–14
  byte pretokens this table sees: `gigatok_cache_bench` clocks FNV at 2.5 ns/hash
  vs 4.3 ns for the "fast" version (0.58×). The byte loop pipelines well and the
  8-byte path pays a variable-length tail `memcpy`. Kept FNV.
- **The real `pair_get` lever was layout, not hashing.** The rank/merged values
  lived in two arrays separate from the keys (struct-of-arrays), so a hit loaded
  from three arrays. Packing `(key, rank, merged)` into one 16-byte entry (AoS)
  makes a hit touch one line, for a stable **~5–7%** on the uncached encode
  (~75 → ~80 MB/s). Marginal here (Qwen's ~4 MB merge table is L3-resident) but
  free and correct, and it scales the way the pretoken-cache packing does.

Absolute numbers are still below gigatoken's per-family-SIMD BPE; the point here
is the *decomposition* — where each speedup lives — at byte-for-byte identical
output. And twice now the honest move was to *measure a plausible optimization
and keep the simpler code* (FNV over a wide hash; SWAR over AVX-512 single-cursor).
