# AGENTS.md — read before changing CNET

## What CNET is building

> **CNET builds ASI — Artificial Specialized Intelligence. In CNET, ASI never
> means Artificial Superintelligence. CNET is not claiming AGI. Broad semantic
> grounding exists to understand intent; deep competence comes from isolated,
> certified, portable Micro Tensor Kernels. Wikipedia-like accumulation may
> broaden coverage and composition over time, but unit count alone is not
> intelligence and all broader claims remain benchmark-gated.**

```
base semantic grounding → typed intent → CNET isolated knowledge registry
      → certified kernels → verifier / abstention → residual teacher (uncovered)
```

If you are about to write "AGI", "general intelligence", "superintelligence",
or "reasoning" about CNET in a doc, commit message, or log marker: don't. Say
which gate measured what, and mark the rest WITHHELD.

## Two things that are NOT the same

| | MTK cartridge (`.tskill` / CMSK / MTSK) | Certified unit / capsule |
|---|---|---|
| Source | `include/cce/cce_mtk.h` | `include/base.h`, `include/cnet_capsule.h` |
| What it is | weight deltas patched onto a host model's named tensor sites | CNU1-sealed BTN + Contract exemplars + ports |
| Typed contract | no | yes |
| Certification | no | yes (behaviour digest, contract verify) |
| Coverage / abstention | no | yes (travels inside the capsule) |
| Portable across bases | only to a matching host model | yes, with explicit compatibility refusal |

Do not equate them, and do not build a second packaging system. Portable
knowledge extends `cnb_export_subset` + the capsule manifest.

## Non-negotiables

1. **Never lower a certification floor to make a gate pass.** Report the failure.
2. **Fail closed, or fail loud.** A silently-degraded guard is worse than a red gate.
3. **No vanity metrics.** A `1.000` must name what was tested and what was not.
4. **Anti-collapse:** never train on CNET's own Tier-A answers. External teacher,
   user correction, or verified tool result only.
5. **AMD/ROCm only.** No CUDA-only paths.
6. **TDD for production behaviour:** failing test with its RED marker first.

## Gates worth knowing

| Gate | Proves |
|---|---|
| `make knowledge_accumulation_bench` | isolation, interference, portable round-trip, corruption/incompatibility refusal, OOD abstention |
| `make knowledge_capsule` | one certified capability transfers with its gate intact |
| `make coverage_abstain` | a unit never answers outside its certified domain |
| `make knowledge_composition_bench` | certified typed composition mechanism at small scale: planner chains 3 independently certified capsules, coverage enforced at **every** hop |
| `make own_learning_health` | refuses dangerous unattended mine/serve configurations |
| `make capability_cert` | held-out capability floors (two graded, not all binary) |

Decision records live in `plans/*.md` — this repo has no separate ADR tree.

<!-- graft:start -->
## Graft — repo context graph

This repo is indexed in `graft/`: small linked markdown nodes that explain each
system and carry exact file:line spans, kept in sync with the code through git.

For ANY task here — understanding how something works, finding where code lives,
or scoping a change — get context from the graph before grepping or opening
source files. Re-ask freely (it's cheap) and reuse literal identifiers you
already have (symbol, error string, file name) as the query. New to this repo?
Run `graft map` first — a token-budgeted orientation (dir clusters, hubs,
hotspots), no LLM, no key.

- Run `graft ask "<your question>" --source` → ranked nodes with the relevant
  code spans inlined (each hit's ≤8-line crux by default; `--full` for whole
  definitions when the crux isn't enough). Match the tool to the task shape:
  for understanding or editing, the top node IS the answer — cite its
  `covers:` file:line spans and edit straight from `--source`. For
  exhaustive tasks ("every occurrence / every caller of this pattern"), ranked
  results are top-N, not complete — run `graft grep "<literal>"` instead
  (exhaustive over indexed files, grouped by enclosing symbol), falling back
  to raw `grep -rn` only for unindexed files.
- `graft skeleton <file>` → every definition's signature + span, ~10× cheaper
  than reading the file; use it to skim an API surface.
- `graft callers <symbol>` gives precomputed, exact edges — who calls this.
  Add `--direction out` for what it calls, or `--depth N` to walk
  transitively for the full blast radius. For structural questions, skip
  ranking and use this directly.
- Or browse: `graft/INDEX.md` lists every node; follow the links.
- Monorepos and folders of multiple repos rank fairly across sub-projects —
  hits carry `[scope/]` labels naming which one they're from. Narrow with
  `graft ask "<task>" --in <scope>/` once you know where you're working.

If a returned span is truncated ("+N more lines"), open the file at that exact
range before finalizing. Only open source files when a node genuinely lacks a
needed detail, and then at the exact file:line the node points to — never
re-read whole files.

After big code changes, refresh the graph with `graft build` (deterministic,
no API key, $0).
<!-- graft:end -->
