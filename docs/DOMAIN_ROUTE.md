# Domain route table (CERT-first)

## Representation

**Both:**

1. **Static C table** compiled into the binary (`k_static_rules[]`)
2. **Optional startup TSV** `config/domain_routes.tsv` loaded into **fixed slots** (`CNET_DR_MAX_RULES=256`)

**Match path:** no `malloc` / `realloc` — scan only, longest pattern within tier.

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
