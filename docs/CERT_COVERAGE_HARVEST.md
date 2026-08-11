# CERT coverage harvest (miss_log → packs)

## Pass

`make cert_coverage_harvest` → **CERT_COVERAGE_HARVEST_PASS**

## What it does

1. Cluster high-value misses (not probe junk)
2. Seed day-0 CERT skills into high-traffic packs via `roe_daily_packs_seed.py`
3. Double-entry: `ROUTES.jsonl` + `config/domain_routes.tsv` + static C rules
4. Gate: harvested phrases → **LOCAL**; zz/mystic probes → **non-LOCAL**
5. Quarantine personal **ABSTAIN-as-CERT** autos (law hygiene)

## Law

- Day-0 seeds = curated CERT for known domains (same as existing packs)
- Live gardener promote still **gold / multi_stable+reviewer only**
- No CERT for probe junk or ABSTAIN text

## Commands

```bash
make cert_coverage_harvest
make roe_daily_packs
make domain_route
```
