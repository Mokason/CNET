# Tokenizer performance gates

The native tokenizer experiments measure different costs:

| Gate | Measures |
| --- | --- |
| `make gigatok_bench` | Pretokenization boundary parity and throughput |
| `make gigatok_encode_bench` | End-to-end encode IDs, merging and cache effects |

[tests/gigatok_bench.c](../tests/gigatok_bench.c) compares scalar and SWAR
variants; [tests/gigatok_encode_bench.c](../tests/gigatok_encode_bench.c)
checks the real tokenizer's fast encode path. Inspect each target before
running it: a real-vocabulary case needs the configured local model/corpus.

Pretokenization GB/s is not full tokenization GB/s. The standalone ASCII-oriented
comparison does not establish arbitrary Unicode-property equivalence.
Real encoder parity must compare exact token IDs for the selected tokenizer.

Cache lifetime changes the result. Per-call cache setup can lose on many
small documents; a persistent cross-call cache can amortize work but must
respect tokenizer ownership and threading constraints. Report cold and warm
runs, corpus repetition, chunk size, thread count and cache memory.

The prior guide's exact tables, unchanged-ID hashes, slower AVX variant and
per-call-cache regression are retained in the [archive](MAINTENANCE.md).
No external tokenizer headline or hardware-independent speedup follows from
these local fixtures.
