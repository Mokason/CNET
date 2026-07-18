# CNET DeepSeek tensor map (forest-native)

## Principle
Python/HF module paths are **import aliases only**.  
Runtime identity is the **forest leaf name** + **contract** (role, dims, residency).

```
FOREST  = whole model
TRUNK   = always-on spine
BRANCH  = decoder layer L
LEAF    = one cascade specialist (named ≤63 chars)
CONTRACT= role + dims + residency + required
```

## Naming (canonical CNET)

| Role | CNET leaf | GGUF alias (import) |
|------|-----------|---------------------|
| Embed | `trunk.embed` | `token_embd.weight` |
| Final norm | `trunk.norm` | `output_norm.weight` |
| LM head | `trunk.head` | `output.weight` |
| Attn RMS | `L03.attn.norm` | `blk.3.attn_norm.weight` |
| MLA Q down | `L03.mla.q_dn` | `blk.3.attn_q_a.weight` |
| MLA KV down | `L03.mla.kv_dn` | `blk.3.attn_kv_a_mqa.weight` |
| MLA KV up | `L03.mla.kv_up` | `blk.3.attn_kv_b.weight` |
| MLA out | `L03.mla.o` | `blk.3.attn_output.weight` |
| FFN router | `L03.ffn.route` | `blk.3.ffn_gate_inp.weight` |
| Expert gate | `L03.ffn.e012.g` | `blk.3.ffn_gate_exps.weight` (bank) |
| Expert up | `L03.ffn.e012.u` | `blk.3.ffn_up_exps.weight` |
| Expert down | `L03.ffn.e012.d` | `blk.3.ffn_down_exps.weight` |

## Residency contracts
| Class | Residency | Why |
|-------|-----------|-----|
| Trunk, norms, MLA core, router | **HOT** | every token |
| Shared expert / MTP | **WARM** | frequent |
| MoE experts | **COLD** | demand-load / weight store |

## API
```c
cce_ds_hparams_default_small(&hp);   /* or parse GGUF meta */
cce_ds_map_build(&map, &hp);
cce_ds_map_validate(&map, err, n);
cce_ds_map_find_cnet(&map, "L00.mla.kv_dn");
cce_ds_map_bind_check(&map, has_gguf_tensor, ctx, &report);
cce_ds_hparams_to_mla(&hp, &mla_cfg);  /* → cce_mla */
```

## Gate
```bash
make deepseek_map   # DEEPSEEK_MAP_PASS
```

## Isolation (no DeepSeek repo, no llama.cpp runtime dep)
| Layer | Owner |
|-------|--------|
| Leaf names + contracts | CNET map |
| Weights at rest | **`.cnetpack`** (CNPK) or synthetic |
| Runtime | **`cce_forest`** cascades |
| GGUF | Optional one-way import (`cce_ds_gguf_load_weight`) via CNET's own reader |

## Real forest bind
```c
cce_ds_bind_opts_default(&opts, "model.cce");
opts.synthetic = 1;          /* or load_weight = cce_ds_pack_load_weight */
opts.bind_cold = 0;          /* experts demand-load later */
cce_ds_map_bind_forest(&map, &forest, &opts, &result);
/* forest->branches named trunk.*, L00.mla.*, L00.ffn.route, … */
cce_forest_get_resident(forest, "L00.mla.kv_dn");
```

## Not Python
No `transformers` modules, no `model.layers.N.self_attn…`.  
Forest names are the contract; GGUF strings are one-way import keys only.
