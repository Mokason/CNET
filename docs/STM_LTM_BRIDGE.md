# STM / LTM bridge (C)

HOT never blocks on COLD. Separate async worker interacts with the producer.

## Map

| Role | Store | API |
|------|--------|-----|
| **STM** | stream index + pins (HOT/WARM tip) | `hot_append`, `pin`, `stm_active` |
| **LTM** | async queue → pager rehydrate or sim COLD | `request_recall` (non-blocking) |
| **CERT** | optional local skill probe | `cert_probe` (not self-CERT from KV) |

```text
HOT decode ──hot_append──► STM index
     │
     ├── request_recall(pos) ──STM hit?──► return immediately
     │                         else ──queue──► worker core
     │                                              │
     │                                    rehydrate COLD / CERT
     │                                              │
     └── poll() ◄────────────── jobs ready ─────────┘
```

## Commands

```bash
make stm_ltm_bridge   # STM_LTM_BRIDGE_PASS
make stm_ltm_bench    # STM_LTM_BENCH_PASS
```

## Law

- COLD KV recall ≠ CERT truth
- Queue full → `CCE_RECALL_BUSY` (backpressure); HOT still runs
- Pins keep STM working set across budget slides
