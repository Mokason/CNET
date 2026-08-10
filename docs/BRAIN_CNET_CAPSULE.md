# Brain → actual CNET capsules

## Law

| Format | What it is |
|---|---|
| Brain `*.cap` / `pieces.bin` | Content-hash linear maps; continuous floats |
| **CNET capsule** | `unit.cnb` + `manifest.cknow` — CNU1 + 0/1 contract + coverage |

CNET contracts are **discrete 0/1**. Continuous Brain `W x + b` cannot be
`unit_save`'d as-is without lowering CNET floors or inventing a second package.

This tool lifts **discrete mode geometry** Brain already learned (peak centers /
mode count) into **real CNET capsules** on the same path as `knowledge_capsule`.

## Units produced

| unit | ports | meaning |
|---|---|---|
| `brain_mode_id` | ONEHOT C → ONEHOT C | certified mode table (identity) from Brain mode count |
| `brain_center_sig` | BINARY_MSB bits → ONEHOT C | center-bit signature → mode (Brain geometry) |

## Commands

```bash
# demo modes (matches Brain 3×10 plant centers)
cd /home/marble/AI/CNET && make brain_cnet_capsule

# from a Brain deploy bundle
cd /home/marble/AI/CNET_Brain && ./build/cb_bundle export /tmp/brain_bundle --train 3000
cd /home/marble/AI/CNET && ./bin/brain_to_cnet_capsule /tmp/brain_bundle artifacts/brain_cnet_capsules_from_bundle
```

## Layout

```
artifacts/brain_cnet_capsules/
  brain_mode_id/{unit.cnb,manifest.cknow}
  brain_center_sig/{unit.cnb,manifest.cknow}
  REPORT.txt
```

## Fail-closed

- CNET floors unchanged
- Import verifies CNU1 seal + coverage + contract
- Continuous serve stays on Brain `pieces.bin` via **opt-in** CNET sidecar
(see § Continuous sidecar). Discrete CNU1 floors unchanged.

## Continuous sidecar (slice 5)

```c
#include "cnet_brain_sidecar.h"
CnetBrainSidecar *sc;
cnet_brain_sidecar_load(&sc, "/path/to/pieces.bin");  // or bundle dir via env
cnet_brain_sidecar_serve_f(sc, x, in, y, out);         // board law + residual hops
cnet_brain_sidecar_free(sc);

/* opt-in process env — never auto-invents continuous answers */
setenv("CNET_BRAIN_SIDECAR", "/path/to/brain_bundle", 1);
const CnetBrainSidecar *e = cnet_brain_sidecar_env();  // NULL if unset/fail
```

```bash
make brain_sidecar   # BRAIN_SIDECAR_PASS
```

| Law | Meaning |
|---|---|
| Opt-in only | no env / no load → no continuous path |
| CBPC v1 | same `pieces.bin` Brain `export_snap` writes |
| Serve | nearest peak + residual α 0.65 LTM (Brain board) |
| Fail-closed | missing/corrupt/dim mismatch → refuse |
| Floors | **does not** admit into CNU1 or lower specialist cert |

## Runtime: Brain loads the *same* CNET capsules

```
CNET authoring (floors, evidence, export)
        │  unit.cnb + manifest.cknow
        ▼
cnet_capsule_step   ← cheap host: unit_load + btn_forward only
cb_hybrid_serve     ← Brain continuous (pieces.bin) + CNET mode capsule
```

```bash
make cnet_capsule_step
./bin/cnet_capsule_step artifacts/.../brain_mode_id --info
./bin/cnet_capsule_step artifacts/.../brain_mode_id --x 0,0,1,0,0,0

# hybrid
cd ../CNET_Brain
./build/cb_hybrid_serve /tmp/brain_bundle --x ... \
  --cnet-capsule ../CNET/artifacts/brain_cnet_capsules_from_bundle \
  --cnet-step ../CNET/bin/cnet_capsule_step
```

Brain does not re-open CNET floors. Continuous Wx+b stays in `pieces.bin`.
