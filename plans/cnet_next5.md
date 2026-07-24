# Next-5 depth (ops + content + decode + cost + CI)

| # | Item | Gate |
|---|---|---|
| 1 | Live smoke recycle + traffic + dashboard | `scripts/cnet_live_smoke.sh` → CNET_LIVE_SMOKE_PASS |
| 2 | 5 procedure chunks sealed + pipeline serve | `make procedure_chunks` → CNET_PROCEDURE_CHUNKS_PASS |
| 3 | Unified decode (picks→labels/jtc) | `cnet_serve_present` / MCP decoded_text |
| 4 | Fault dedupe + teach cost counters | CNET_FAULT_PASS dedupe; acct peft_train/dedup_skip |
| 5 | verify-fast vs verify-nightly | `make verify-fast` / `make verify-nightly` |

```bash
make verify-fast
make procedure_chunks
make cnet_next5   # smoke + library + dashboard
# nightly: make verify-nightly
```
