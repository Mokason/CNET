# SSMax + Full DSA + MLA-lite — single-user path

## Limits solved

| Prior limit | Solution |
|-------------|----------|
| Index = raw q·k only | **Lightning indexer**: multi-head ReLU (+ hybrid) |
| No compressed KV | **MLA-lite** int8 + full **DeepSeek MLA** engine (`cce_mla`, `make mla`) |
| Floor hurts quality | **Quality default**: sleep only; floor via `speed` profile |
| Datacenter tok/s stack | Skip EP/disagg; use DSA+MoE skip+int8+optional GPU |

## DeepSeek mapping (local)

| DeepSeek | CNET single-user |
|----------|------------------|
| Lightning indexer | `cce_dsa_lightning_index*` (ReLU multi-head / hybrid) |
| Top-k select | `cce_dsa_select_dual` (index = WHO, q·k = HOW) |
| Sparse attend | Skip zero-mass V; optional int8 dequant only on support |
| MLA latent KV | `cce_mla` (latent cache + decoupled RoPE + absorb) + optional int8 lite |
| MoE sparse | SSMax topk + sleep skip experts |
| Wide-EP / dual-batch | Not needed (one user) |

## Env

```bash
CNET_DSA=1                 # enable DSA @ 25% support
CNET_SPARSE_KV=0.25        # or set fraction explicitly
CNET_DSA_PROFILE=speed     # enable floor grid (more skips)
CNET_DSA_FLOOR=0.001       # optional override
CNET_DSA_SLEEP=0.0001
CNET_MLA_KV=1              # int8 compressed KV side cache
```

## Gates

```bash
make dsa ssmax sparse_kv_exec moe_forward moe_stream
```
