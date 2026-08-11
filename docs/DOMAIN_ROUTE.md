# Domain route table (CERT-first)

## Representation

**Both:**

1. **Static C table** compiled into the binary (`k_static_rules[]`)
2. **Optional startup TSV** `config/domain_routes.tsv` loaded into **fixed slots** (`CNET_DR_MAX_RULES=256`)

**Match path:** no `malloc` / `realloc` — scan only, longest pattern **within tier**.

## Match policy (B + C)

| Policy | Rule |
|--------|------|
| **B min length** | `CNET_DR_DEFAULT_MIN_PAT` (4) or per-rule `min_pat_len` |
| **C word boundary** | pattern must sit on non-alnum edges (no `format` inside `unformatted`) |
| **conf_x1000** | telemetry only — does not gate fall-through |

## Dispatch chain

```text
CERT pack → MTK .tskill → Base GGUF → Abstain
```

Fail-closed default: **ABSTAIN** (no MTK, no KV flush).

Front door prints the decision but **never auto-applies MTK** (CERT path stays primary).

## Commands

```bash
make domain_route
./bin/roe_domain_route "who are you"
./bin/roe_domain_route "completely unknown domain xyzzy"
```
