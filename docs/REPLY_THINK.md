# Visible thinking on reply (0 tokens)

Like GPT’s “thinking” UI — **without spending model tokens**.

## User sees

```text
┌─ thinking (0 tokens) ──────────────────
│ User asked: who are you
│ Body state: DA=… 5HT=… ADO=…
│ Intent: identity_local
│ Route: pack=pack_soul_marble skill=soul_who
│ Plan: load_pack_soul_marble → answer_LOCAL_soul → …
│ Outcome path: source=LOCAL skill=soul_who (0 teacher tokens)
│ Checks: never self-CERT; …
│ Continuity: continuity: marble | …
│ Note: 0 LLM tokens. Not consciousness.
└────────────────────────────────────────
answer:
I am Marble — …
```

Markdown (`--style md`) uses a collapsible `<details>` block (chat UIs).

## Commands

```bash
make cnet_reply_think
# full reply = front_door answer + thinking panel
scripts/roe_reply.sh "who are you"
scripts/roe_reply.sh "who are you" --md

# thinking only / inject answer
python3 scripts/cnet_reply_think.py --query "..." --ask
python3 scripts/cnet_reply_think.py --query "..." --answer "..." --source LOCAL --skill x
```

## Efficiency vs GPT CoT

| | GPT thinking | CNET reply think |
|--|--------------|------------------|
| Cost | burns completion tokens | **0** |
| Content | freeform model prose | structured ops trace |
| Truth | can invent | state + routes + law only |
| Promote | N/A | never from thinking alone |

## Files

- `scripts/cnet_reply_think.py`
- `scripts/roe_reply.sh`
- `logs/governor/reply_think_last.json` / `.md`

## Law

`llm_thinking: false` · `not_conscious` · `never_self_cert` · thinking ≠ CERT
