# CNET vs transformer (fail-closed QA), 24 questions

Score = correct - 2 * wrong. Abstain = 0. Gold = held-out probe or corpus sentence.
CNET answer = nearest sealed teacher sentence in the routed capsule, not n-gram generation.

| system | n | correct | wrong | abstain | score |
|---|---|---|---|---|---|
| CNET capsules (route + retrieve) | 24 | 6 | 10 | 8 | -14 |
| transformer | - | - | - | - | WITHHELD (no --transformer-url / CNET_VS_TRANSFORMER_URL) |

WITHHELD: transformer lane. Mouth :8084 is a story generator, not a QA endpoint,
and the 27B teacher that wrote the gold is not a legal baseline.

WITHHELD: open-ended language; energy; multi-hop composition.
