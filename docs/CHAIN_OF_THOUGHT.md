# Chain-of-thought (C)

**Language: C only** — `include/cnet_roe_cot.h`, `src/cnet_roe_cot.c`, `tools/roe_chain_think.c`.  
No Python on this path. (Older Python thought helpers may still exist; CoT product path is C.)

## Shape

```text
PARSE → CHECK → SPLIT → (RETRIEVE → ACT)×N → JOIN → VERIFY → SHOW
```

- Skeleton hops: **0 LLM tokens**
- ACT: `./bin/roe_front_door` (C)
- Teacher on miss: only with `--teacher`
- Neuromod from `logs/governor/*.json` (ADO stop / depth)
- Law: never_self_cert · not_conscious · not_agi · chain≠CERT

## Commands

```bash
make roe_chain_think
./bin/roe_chain_think "who are you"
./bin/roe_chain_think --no-act "plan only"
./bin/roe_chain_think --teacher "hard miss query"
scripts/roe_reply.sh "who are you"    # bash + C only
```

## Output

```text
┌─ chain-of-thought (0 tokens*) ─────────
│ hop1 PARSE ...
│ hop2 CHECK DA/5HT/ADO ...
│ hop3 SPLIT ...
│ hop4 RETRIEVE pack=...
│ hop5 ACT front_door LOCAL ...
│ hop6 JOIN ...
│ hop7 VERIFY never_self_cert
│ hop8 SHOW ...
└────────────────────────────────────────
answer (LOCAL):
...
```

Files: `logs/governor/chain_last.txt`, `chain_last.json`, `chain_think.jsonl`
