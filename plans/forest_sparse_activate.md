# Forest-native sparse activate (MLA + DSA + MoE + quant KV)

## Principle

Keep **Forest specialists / sleep / router** as the product identity.
DeepSeek-style speed is a **fire policy** on those leaves — not a second runtime.

```
soul / task route     → unit / forest
SSMax branch router   → specialist family
MoE FFN top-k+sleep   → Lxx.ffn.eNNN.* cold leaves
DSA token sleep       → which latent KV positions get V work
MLA latent cache      → compressed c_kv (+ optional int8 side)
DualPipe-like         → ensure support (load) then fire (compute)
```

## Mapping

| DeepSeek | CNET Forest form |
|----------|------------------|
| MLA | `cce_mla` + absorb; leaves `Lxx.mla.*` |
| FlashMLA-class BW | `cce_mla_enable_quant_kv` — int8 latent (1 B/elem, FP8-class) |
| DSA | lightning-ish index + `cce_dsa_select_dual` + sleep on support |
| MoE sparse | `ffn.route` → `cce_ssmax_topk_weights` → `cce_sleep_renorm` → cold ensure → fire |
| Shared expert | optional leaf `Lxx.ffn.shared.{g,u,d}` always on |
| DualPipe | phase1 ensure all awake experts; phase2 fire (ready for multi-GPU place) |

## Env

```bash
CNET_DSA=0              # disable DSA (default on in ds host)
CNET_DSA_PROFILE=speed  # floor grid
CNET_DSA_SLEEP=1e-4
CNET_MLA_KV=0           # disable int8 latent (default on)
CNET_MOE_SLEEP=1e-4
CNET_DUAL_PIPE=0        # disable batch-ensure phase
```

## Gates

```bash
make sparse_stack   # ssmax dsa mla deepseek_map ds_stack
make cnet_ds_bench  # synthetic tok/s + experts_fired
```

## Telemetry (`cce_ds_host`)

- `experts_loaded` — cold → resident lifetime
- `experts_fired` / `experts_slept` / `experts_attempted`
- `dsa_support_sum` / `dsa_full_fallback`
- `mla_quant_kv` / per-layer `cache.quant_kv`

## Isolation

Pure C, no DeepSeek repo, no llama.cpp on this path. GGUF import optional into `.cnetpack` + forest names.

## MTP speculative + EP place (Forest branches)

```text
trunk.head          main next-token head (verify)
draft.mtp.0         cheap draft head (+ optional 1-layer draft trunk)
embeds              synthetic token inject into residual
expert_place[e]     e % ep_places  (dual-GPU role tags for MoE leaves)
```

**Loop:** draft proposes k tokens → restore snapshot → for each token, main head must agree **before** inject; then full trunk forward; on mismatch commit main's token and stop.

```bash
make mtp_spec          # hermetic
make mtp_bench         # baseline vs MTP wall tok/s
# CNET_MTP_K=2 CNET_MTP_DRAFT_LAYERS=0 CNET_EP_PLACES=2
```

Honest note: sequential verify still pays ~1 main step per accepted token; wall speedup needs parallel multi-token verify or a much smaller draft (draft_layers=0 is near-free). Machinery + accept_rate telemetry land first.
