# Grok evolve guide (non-CERT)

Status: **OPTIONAL residual guide**

During `cnet_core_evolve`, if `CNET_GROK_GUIDE=1`:

1. Read `$BRICKS/guide_questions.txt` (also filled from Obsidian `cnet_guide:`)
2. Ask Grok up to `CNET_GROK_GUIDE_MAX_Q` (default 3)
3. Hard wall clock per question: `CNET_GROK_GUIDE_SECONDS` default **1800 (30 min)**, clamp ≤1800
4. Write answers to `guide_log.md` only
5. **Never** auto-CERT from Grok output

Requires `XAI_API_KEY` (or `XAI_KEY` / `GROK_API_KEY`).
