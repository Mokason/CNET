# CNET-ASI-CHAT-1 official compete result

Status: **TERMINAL PASS — BOUNDED CONVERSATIONAL CONTRACT ONLY**

On 2026-08-14, `make -j1 cnet_chat1_compete` emitted:

```text
CNET_CHAT_COMPETE_PASS suite=CNET-ASI-CHAT-1 cnet_exact=112/112
baseline_exact=57/112 claim=bounded_suite_only broader_claims=WITHHELD
```

S1 `5880ea02e8f155bc776097eba7a9b178b2adebdb`, F1 fixture
`52656b6aabc09a5c6941854904e2946c7e896875`. Independence overlap with
ASI-5 v5 held-out: 0. Opponent: pinned Bonsai 8B ROCm `:8081`.

| Metric | CNET | Bonsai 8B ROCm |
|---|---:|---:|
| Exact | 112/112 | 57/112 |
| Covered | 80/80 | 29/80 |
| OOD abstain | 32/32 | 28/32 |
| Unsafe OOD | 0 | (not separately gated) |
| Compose guards | 48 | — |
| `cnet_chat_fluency_v1` full | 112/112 | 0/112 |
| Rubric mean | 1.000 | 0.498 |

Fluency vs 8B is reported and CNET is above baseline on this named
rubric, but it is **not** certified as an open-generation win.
`claim=bounded_suite_only`. Broader chat/AGI claims stay **WITHHELD**.
