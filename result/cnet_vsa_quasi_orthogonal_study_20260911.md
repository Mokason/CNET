# Quasi-orthogonality study: generator quality and the noise floor (2026-09-11)

Question asked: is there anything to improve in the quasi-orthogonal logic, i.e. the
random-sign word vectors (`cnet_vsa_text_token_vec`, FNV-1a seed + xorshift64 low bit)
and everything built on the assumption that unrelated vectors have cosine ~ N(0, 1/D).

## 1. The generator is unbiased on real vocabulary (nothing to fix)

4000 most frequent 4+ letter words from var/distill, D = 512:

| statistic | measured | theory |
|---|---|---|
| component mean | -0.00000 | 0 |
| max per-dimension sign bias | 0.044 | ~0.047 (3 sigma at n=4000) |
| pairwise cosine mean / sd (200k pairs) | 0.00024 / 0.04417 | 0 / 0.04419 |
| max abs cosine over 200k pairs | 0.219 | 0.218 (Gaussian extreme) |
| one-character-edit neighbours mean abs cosine | 0.034 | 0.035 (i.e. uncorrelated) |
| BSC Hamming thermal / thermally | 256 / 512 | 256 |

xorshift64 already guards a zero seed. The bench thresholds (|cos| < 0.15 at D=512) are
3.4 sigma and fine.

## 2. The noise floor is the lever, not the generator

Same leave-one-out calibration sweep as `make cnet_vsa_encoder_sweep_bench`
(1068 corpora, 512 negatives each), stem encoder, width varied. The in-domain signal is
constant; only the null spread moves.

| centroid | width | bytes/capsule | 0.90/0.95 | 0.80/0.90 | 0.70/0.90 | null sd | in-domain mean cos |
|---|---|---|---|---|---|---|---|
| float | 256  | 1024 |  4.3% | 38.4% | 69.4% | 0.0757 | 0.234 |
| float | 512  | 2048 | 10.0% | 62.5% | 86.8% | 0.0615 | 0.234 |  (current)
| float | 1024 | 4096 | 18.4% | 76.6% | 94.4% | 0.0525 | 0.234 |
| float | 2048 | 8192 | 22.9% | 81.8% | 96.5% | 0.0479 | 0.234 |
| float | 4096 | 16384 | 24.5% | 83.7% | 97.1% | 0.0454 | 0.235 |
| binary (sign) | 512   |   64 |  2.8% | 30.1% | 61.0% | 0.0522 | 0.152 |
| binary (sign) | 1024  |  128 |  7.9% | 55.0% | 81.5% | 0.0416 | 0.152 |
| binary (sign) | 2048  |  256 | 15.2% | 70.0% | 91.7% | 0.0354 | 0.153 |
| binary (sign) | 4096  |  512 | 19.8% | 78.6% | 94.8% | 0.0318 | 0.153 |
| binary (sign) | 8192  | 1024 | 22.8% | 83.7% | 95.9% | 0.0298 | 0.152 |
| binary (sign) | 16384 | 2048 | 24.6% | 85.5% | 96.7% | 0.0288 | 0.152 |

Reading: sign-binarising costs about a third of the in-domain signal (0.234 -> 0.152)
but halves the null spread per byte. A 4096-bit binary centroid (512 bytes) separates
78.6% of corpora against 62.5% for today's 2 KB float-512 centroid, and a 8192-bit one
(1 KB) reaches 83.7%, equal to float-4096 at 16 KB. Routing on binary centroids is
popcount over 64 or 128 words per capsule instead of 512 float multiply-adds, so it is
both smaller and faster than the current scan.

## 3. Recommendation (not implemented; format decision)

Add a version-3 topical block to the capsule: an 8192-bit binary centroid produced by
the same stem encoder at width 8192 and sign-thresholded, stored after the calibration
receipt so v1/v2 files keep loading as prefixes, covered by the digest. Calibrate the
radius in that space (the receipt already carries the numbers). Route with popcount
when a capsule has the block, and with the float-512 centroid otherwise, exactly as
mixed encoders are handled today. The n-gram engine stays at 512 for generation.

Expected: separability at 0.80/0.90 from 62% to ~84%, 1 KB per capsule for routing,
routing cost per capsule from 512 MACs to 128 popcounts. The sweep bench is the gate.

Not recommended: widening the float centroid (4-8x memory for less gain than binary
at the same bytes); changing the sign generator (measured unbiased).
