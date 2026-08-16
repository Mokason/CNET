# Gold / curriculum harvest (Step 2)

## Goal

Secure **inflow** to the gardener: gold files + curriculum only for verified answers;
block probe junk and ABSTAIN-as-CERT at **promote**.

## Commands

```bash
make gold_curriculum_harvest   # smoke + gold + curriculum + blocklist
python3 tools/roe_evolve_tick.py --dry-run
```

## Law

| Artifact | Auto-CERT? |
|----------|------------|
| `gold/<sha>.txt` | No — external verify for `gold_file` promote |
| `curriculum_harvest.jsonl` | No — feed/hints only |
| `pack_personal` skill | Only via evolve: gold_file **or** multi_stable+reviewer |
| blocklist hit | **Never** promote |

## Blocklist

`config/promote_blocklist.txt` — substr / answer_prefix / regex.  
Wired in `tools/roe_evolve_tick.py` → `is_promote_blocked()`.
