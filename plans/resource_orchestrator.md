# Resource orchestrator (duty-cycle compute)

## Problem

Opening a model and generating often pins GPU/CPU at **90–100%** for the
whole session — even between tokens, after answers, or when sparse work
would suffice. That is wasteful for a single-user local AI.

## Solution

Extend `resource_governor` with a **compute orchestrator**:

1. **Profiles** resolve sparse / page / MTP knobs *before* model load  
2. **Phases** track IDLE → WARM → GENERATE → DRAIN → IDLE  
3. **Pacing** (`min_token_gap_ms`) yields between tokens (eco)  
4. **Cool** after generate: signal host to drop teacher / not spin  

Budget API (RAM/VRAM/teach rate) is unchanged and still fail-closed.

## Profiles

| Profile | Intent | DSA | HOT pages | Pace | Rehydrate |
|---------|--------|-----|-----------|------|-----------|
| **eco** | Min heat | 0.20 + speed floor | 2 | 2 ms | off |
| **balanced** | Default | 0.25 sleep-only | 4 | 0 | off |
| **turbo** | Max tok/s | 0.50 | 8 | 0 | on |

## API (C)

```c
CnetResourceGovernor g;
cnet_gov_orchestrate_boot(&g, "eco");   /* or CNET_GOV_PROFILE=eco */

/* AFTER this, load GGUF / open ds_host — they read CNET_* env */

cnet_gov_begin_generate(&g);
for each token:
    /* forward ... */
    cnet_gov_between_token(&g);         /* optional pace sleep */
cnet_gov_end_generate(&g);              /* cool recommended? */

if (cnet_gov_tick_idle(&g, idle_sec))
    /* unload teacher / release GPU */
```

## Env

```bash
export CNET_GOV_PROFILE=eco          # eco | balanced | turbo
export CNET_GOV_FORCE=1              # overwrite existing CNET_* 
export CNET_GOV_TOKEN_GAP_MS=5       # override pace
export CNET_GOV_IDLE_COOL_MS=500
export CNET_GOV_DSA_FRACTION=0.25
export CNET_GOV_KV_PAGE=1
export CNET_GOV_MTP_K=2
# plus existing: CNET_GOV_RAM_BYTES, CNET_TEACHER_IDLE_SEC, ...
```

`apply_compute_env` sets (if unset, unless FORCE):

`CNET_DSA`, `CNET_SPARSE_KV`, `CNET_DSA_PROFILE`, `CNET_MLA_KV`,
`CNET_KV_PAGE`, `CNET_KV_HOT_PAGES`, `CNET_KV_PAGE_LEN`, `CNET_KV_QUANT`,
`CNET_KV_REHYDRATE`, `CNET_KV_ASYNC`, `CNET_MTP_K`, `CNET_MTP_PARALLEL`,
`CNET_EP_PLACES`.

## Gate

```bash
make resource_governor   # RESOURCE_GOVERNOR_PASS
```

## What this is not

- Not a replacement for OS cgroups / nvidia-smi power caps  
- Not mid-GEMM throttling (saves work by **skip + duty cycle**)  
- Hosts must call `begin/between/end` — pure loaders that ignore the
  governor still need a one-line boot before open  

## Wire-up checklist (hosts)

1. `cnet_gov_orchestrate_boot` at process start  
2. Model load  
3. Wrap generation loops with begin / between / end  
4. On idle timer: `tick_idle` → teacher sleep / GPU release  
