# Pattern runtime — fluid → freeze → contract callout

## Idea

Not a 2D N→blackbox→M net. A **4D pattern space**:

| Axis | Field |
|------|--------|
| code | `CnetPatternAddr.code[]` (discrete-ish digests) |
| role | port tag / lane |
| time | version + state (fluid / improving / frozen / demoted) |
| place | residency hot slots (LFRU unload) |

Edges are explicit connections (like synapses, but **callouts**):

```text
in_addr ──body──► out_addr
body ∈ unit | tool | math | skill | hermetic
state: fluid → improving → frozen (absolute enough)
```

Learning = propose fluid edges, improve with feedback, **freeze** when reliable.  
Truth = frozen edge invoked by contract callout.

## API

```c
cnet_pattern_runtime_init / from_env / load / save
cnet_pattern_propose      /* fluid edge */
cnet_pattern_feedback     /* success/fail → may freeze */
cnet_pattern_freeze / demote
cnet_pattern_load / unload / tick   /* residency */
cnet_pattern_find / callout
cnet_pattern_observe_math / observe_skill
cnet_pattern_bootstrap_defaults
```

## Wire-in

- `cnet_math_solve` on verified answer → `observe_math` + save store  
- CLI: `bin/cnet_pattern callout "what is 6*7"`  
- Store: `logs/pattern_runtime.jsonl` (or `CNET_PATTERN_STORE`)
- **SoulHost units:** `import-units` via dlopen `cnet.so` + `CNET_BASE_PATH` → frozen `UNIT` edges; callout runs `soul_run`
- **Ops tick:** `OPS_PATTERN_PROMOTE=1` → `cnet_pattern promote`; optional `OPS_PATTERN_IMPORT_UNITS=1`
- **Curriculum:** materialize success → `cnet_pattern propose <kind> <text>` fluid edges

## Gates

```bash
make pattern_runtime   # PATTERN_RUNTIME_PASS
make math_solve        # still green; feeds patterns
```

## Files

- `include/cnet_pattern.h`
- `src/cnet_pattern.c`
- `tests/test_pattern_runtime.c`
- `tools/cnet_pattern.c`
