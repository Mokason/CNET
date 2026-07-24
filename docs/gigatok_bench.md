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
| SWAR + dual, 32 threads | **~24 GB/s** | in gigatoken's 20–24 GB/s range |

**Confirmed:** the single-thread SWAR+dual pretokenizer really does cross
~1 GiB/s/thread; the dual-cursor ILP really does add ~25–30%; and it scales to
~24 GB/s aggregate — i.e. gigatoken's *core throughput mechanism* reproduces
independently in C.

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

## End-to-end: wired into cce_gguf_tok (`make gigatok_encode_bench`)

The SWAR pretokenizer and a **pretoken cache** are wired into the real BPE encode
path as `cce_gguf_tok_encode_fast(t, text, ids, max, flags)`
(`CCE_TOK_FAST_SWAR | CCE_TOK_FAST_CACHE`), leaving `cce_gguf_tok_encode`
untouched. The SWAR skip runs inside the *same* pretok grammar (non-ASCII and
apostrophes still go through the exact codepoint logic), and the cache memoizes
`span -> token-ids`; both are exact, and `tests/gigatok_encode_bench.c` asserts
the fast path's ids equal the baseline **byte-for-byte**.

Measured on a real GGUF vocab (Qwen-family, 248k tokens; ~4 MB corpus; same
pattern on gemma 262k):

| config | throughput | vs baseline | ids |
|---|---:|---:|---|
| baseline (`cce_gguf_tok_encode`) | 19 MB/s | 1.00× | — |
| + SWAR pretok | 19 MB/s | **1.00×** | identical |
| + pretoken cache | 273 MB/s | **14.4×** | identical |
| + SWAR + cache | 305 MB/s | **16.0×** | identical |

**The honest finding — and it matches gigatoken's own thesis.** In this encoder
(as in most) `bpe_word` is the bottleneck, not pretokenization: SWAR pretok alone
buys **~0% end-to-end**. The **pretoken cache is the whole win (14×)** — it skips
`bpe_word` on the ~99% of pretokens that repeat in natural text. SWAR only starts
to matter *after* the cache removes the BPE cost (then it adds ~11%, since it's
now the remaining bottleneck). That's exactly why gigatoken needed both, and why
its README calls caching "a very hard problem in this domain."

Caveats: the cache is per-`encode_fast`-call; the bench encodes the corpus in one
call, so it reflects the within-a-large-encode hit rate — a persistent
(cross-call) cache would be the streaming-many-documents extension. Our absolute
numbers are gated by a deliberately naive `bpe_word` (malloc + `strlen` per
merge); the point here is the *decomposition* (where the speedup lives), not
beating gigatoken's absolute GB/s.
