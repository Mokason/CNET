# CNET paragraph invent — program goal

Status: **GOAL + UNIT GATE**

A paragraph is several native sentences from a **certified table**.
It is not residual GGUF, not WordLM decode, and not a phrase-bank blob.
Broader / open-generation claims stay **WITHHELD**.

```
wrap → closed-vocab slots → unique table row or abstain
     → native paragraph (≥2 sentences) from bound slots only
```

Floors: ASI-5 and CHAT-1 untouched. Residual = 0. OOD abstain = 1.
Anti-collapse: do not train on these spoken lines.

First table: in-house CamRest-shaped `restaurant_inform_v1` (not an HF dump).

```text
make cnet_paragraph
```
