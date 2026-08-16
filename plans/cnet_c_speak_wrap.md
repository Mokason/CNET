# DRAFT MOUTH — WRAP ONLY

Status: **SLICE 1 — C SPEAKS GLUE, A ASSERTS SLOTS — OPEN CHAT WITHHELD**

This is not open chat. This is not beat-8B. This is not F11. Broader
claims stay **WITHHELD**.

`tools/cnetd.c:cd_ask` already binds a lookup hop
(`cnet_chat_lookup_cnetd_hop`) before residual / teacher-on-miss. Slice 1
lets the live native generative leaf draft the sentence that *wraps* that
bound value. A supplies every asserted slot. C supplies the glue words.

## Generative leaf actually called

`cce_wordlm_predict` in `src/cce/cce_wordlm.c` (the ARCHITECTURE.md
word-level free-run leaf). Call site: `src/cnet_c_speak.c` `generate_tokens`
→ `cce_wordlm_predict`.

This is **not** the 321,757-parameter compete intent WordLM. That
classifier is not grown and is not this mouth.

A tiny wrap instance is trained in-process on function-word glue with
slot tokens (`<val>`, `<host>`, `<contract>`). No ASI-5 448 / held-out /
excluded_prompts row bodies. No F11 journals.

## Hard switch

- A bound → wrap A. Residual stays 0.
- Nothing bound → refuse / fall-through. No CERT claim. No invented number.
- Teacher-on-miss drafts may still be logged. They are not spoken.
- `never_voice_llm` stays on. Residual / Teacher / 8B / GGUF is never the mouth.
- Soft router / Rosenbaum mix over CERT is forbidden.
- Generative text is not auto-CERTed.

## Hook

`cd_ask` after a bound lookup hop calls `cnet_c_speak_after_lookup` (same
function the unit test calls). If the leaf wrap carries the exact A value
and `residual_calls == 0`, that line is spoken. Otherwise the existing
A-only lookup speak remains (lookup already had a mouth).

A verified LOCAL/CERT capsule scalar (short slot, not prose) may also be
wrapped via `cnet_c_speak_after_capsule`. Teacher source is excluded.

## What this is not

- Not open chat. Not a chatbot.
- Not beat-transformer / beat-8B. Fluency-vs-8B stays **WITHHELD**.
- Not F11. Floors / 448 / CHAT-1 scores are not claimed improved.
- Not a template farm pretending to be C-speak. Lookup's
  `cnet_lookup_speak` snprintf remains the A-only fallback.
- Not hooked into fluency or compete. No libcurl added there.

## What is still missing

- Grow the leaf (larger wrap vocab, longer honest drafts).
- Frozen CHAT arena / official `CNET_CHAT_COMPETE_PASS`.
- Uncertified no-bind drafts that cannot be mistaken for CERT
  (slice 1 refuses that path).
- Capsule wrap for long CERT prose (identity/status stay on utterance).

## Gate

```text
make cnet_c_speak
CNET_C_SPEAK_PASS
checks=58 leaf=cce_wordlm_predict residual=0 broader_claims=WITHHELD
```
