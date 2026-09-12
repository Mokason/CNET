# Retrieval latency correction — 2026-09-12

The user rejects 430 microseconds and sets a maximum of 25 microseconds for the
full query path. The earlier 1 ms provisional budget is superseded; the earlier
4–7 microsecond number measured encoding only. No production promotion.

Profile of the previous query path: median fusion 107 us, passage 92 us, dense
scan 56 us, WordPiece 24 us. Its full p95 reproduces at 424 us.

Implement and measure a native sparse query path. Reuse the CNET tokenizer and
compile the external model's whole-word semantic vocabulary into stem-keyed
postings offline, max-pooling aliases per capsule. This changes retrieval
semantics; report accuracy across all eight old blocks and the 773 control.
No evaluation text/labels choose index entries or ranking weights.

Fixed presets before evaluation: semantic only, equal RRF of semantic and
expanded BM25, and the same RRF plus 20% source passage coverage/order. Test
256/1024 original semantic terms per capsule. Optional centroid candidate
scoring is conditional on the profile leaving room under 25 us; never hide
its work outside the timer. Rank output, tokenization, and Python call overhead
must be included. Report p50/p95/max; 25 us is a budget, not a hard realtime
OS guarantee. Do not call a p95 pass an absolute maximum guarantee.

Keep all original result records and protected source/fixtures unchanged.
Use new experiment files. RED tests for native sparse equivalence, rank tie
order, passage equivalence, and invalid input must precede their implementation.
