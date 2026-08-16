# CNET-Minimal — Install & smoke

Versioned Autonomous-ASI front door runtime (C + CERT packs).  
**Never self-CERT.** Residual GGUF optional; teacher secrets not included.

## Layout

```text
CNET-Minimal-<ver>/
  bin/           roe_front_door, roe_domain_route, roe_chain_think, cnet-ask, …
  data/          roe_daily_packs/ (CERT seeds)
  config/        domain_routes.tsv, promote_blocklist.txt, *.env.example
  tools/         roe_evolve_tick.py (python3)
  scripts/       cnet_runtime_smoke.sh, cnet_runtime_soak_gate.sh
  INSTALL.md
  VERSION
```

## Install

From repo:

```bash
make cnet_minimal_package
# → dist/CNET-Minimal-<gitsha>/
# → dist/CNET-Minimal-<gitsha>.tar.gz
```

Or unpack a tarball:

```bash
tar -xzf CNET-Minimal-*.tar.gz
cd CNET-Minimal-*
```

Requirements: Linux x86_64, `python3` (for evolve), optional curl for teacher.

## Smoke

```bash
export CNET_MINIMAL_ROOT=$PWD
./scripts/cnet_runtime_smoke.sh
# expect: CNET_RUNTIME_SMOKE_PASS
```

## Front door

```bash
./bin/cnet-ask "who are you"
./bin/roe_front_door ask "format-truncation werror" --root "$PWD/data/roe_daily_packs"
./bin/roe_domain_route "cnet never lowers floors for brain floats"
```

## Evolve (optional)

```bash
# from repo tree (uses artifacts/ symlink or run in-repo)
python3 tools/roe_evolve_tick.py --dry-run
# promote only gold_file | multi_stable+reviewer; blocklist drops probes/ABSTAIN
```

Copy `config/teacher.env.example` / `reviewer.env.example` and source them for live teacher/reviewer — never bake secrets into the package.

## Soak gate (release)

```bash
make cnet_runtime_soak_gate
# CNET_RUNTIME_SOAK_GATE_PASS
```

Assertions: no ABSTAIN-as-CERT in personal; blocklist skips probes; local_hit ≥ 0.80 when autonomous report present.

## Law

- CERT packs = fail-closed LOCAL answers  
- `.tskill` / MTK = residual only  
- Stream index = pre-attention HOT mask when residual GGUF bound  
- Promote path: gold or multi_stable+reviewer only  
