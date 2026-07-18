/* CNET-native DeepSeek forest tensor map — make deepseek_map → DEEPSEEK_MAP_PASS */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_deepseek_map.h"
#include "../include/cce/cce_mla.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Fake GGUF presence: all trunk + layer0 MLA + router present; experts absent */
static int fake_has(void* ctx, const char* name) {
    (void)ctx;
    if (!name) return 0;
    if (strstr(name, "token_embd") || strstr(name, "output") ||
        strstr(name, "attn_") || strstr(name, "ffn_norm") ||
        strstr(name, "ffn_gate_inp"))
        return 1;
    if (strstr(name, "ffn_gate_exps") || strstr(name, "ffn_up_exps") ||
        strstr(name, "ffn_down_exps"))
        return 0; /* missing experts → required miss for MoE */
    return 0;
}

int main(void) {
    cce_ds_hparams hp;
    cce_ds_map map;
    char err[128];
    int hot, warm, cold;

    printf("== CNET DeepSeek forest tensor map ==\n");

    cce_ds_hparams_default_small(&hp);
    check(cce_ds_map_build(&map, &hp) == CCE_OK, "build small map");
    check(cce_ds_map_validate(&map, err, sizeof err) == CCE_OK, "validate map");
    printf("    leaves=%d trunk=%d attn/L=%d ffn/L=%d\n",
           map.n_leaves, map.n_trunk, map.n_attn_per_layer, map.n_ffn_per_layer);

    /* Trunk names */
    check(cce_ds_map_find_cnet(&map, "trunk.embed") != NULL, "trunk.embed");
    check(cce_ds_map_find_cnet(&map, "trunk.norm") != NULL, "trunk.norm");
    check(cce_ds_map_find_cnet(&map, "trunk.head") != NULL, "trunk.head");

    /* Layer leaf names (short, forest-safe) */
    check(cce_ds_map_find_cnet(&map, "L00.mla.kv_dn") != NULL, "L00.mla.kv_dn");
    check(cce_ds_map_find_cnet(&map, "L00.mla.kv_up") != NULL, "L00.mla.kv_up");
    check(cce_ds_map_find_cnet(&map, "L00.ffn.route") != NULL, "L00.ffn.route");
    check(cce_ds_map_find_cnet(&map, "L00.ffn.e000.g") != NULL, "L00.ffn.e000.g");
    check(cce_ds_map_find_cnet(&map, "L01.mla.o") != NULL, "L01.mla.o");

    /* GGUF alias reverse lookup */
    {
        const cce_ds_leaf* L =
            cce_ds_map_find_gguf(&map, "blk.0.attn_kv_a_mqa.weight");
        check(L != NULL && L->role == CCE_DS_ROLE_MLA_KV_DOWN,
              "gguf alias → MLA_KV_DOWN");
        check(strcmp(L->cnet, "L00.mla.kv_dn") == 0, "alias maps to CNET name");
    }

    /* Role + layer find */
    {
        const cce_ds_leaf* L =
            cce_ds_map_find_role(&map, CCE_DS_ROLE_FFN_ROUTER, 1, -1);
        check(L && strcmp(L->cnet, "L01.ffn.route") == 0, "role find router L1");
    }

    /* Name length budget */
    {
        int i, ok = 1;
        for (i = 0; i < map.n_leaves; ++i)
            if ((int)strlen(map.leaves[i].cnet) >= 64) ok = 0;
        check(ok, "all cnet names < 64 (branch.name budget)");
    }

    /* Residency: trunk+MLA hot, experts cold */
    cce_ds_map_residency_counts(&map, &hot, &warm, &cold);
    check(hot > 0 && cold > 0, "has HOT and COLD leaves");
    printf("    residency hot=%d warm=%d cold=%d\n", hot, warm, cold);
    {
        const cce_ds_leaf* e =
            cce_ds_map_find_cnet(&map, "L00.ffn.e001.d");
        check(e && e->res == CCE_DS_RES_COLD, "expert is COLD contract");
        const cce_ds_leaf* k =
            cce_ds_map_find_cnet(&map, "L00.mla.kv_dn");
        check(k && k->res == CCE_DS_RES_HOT, "MLA kv_down is HOT");
    }

    /* Dim contracts for MLA */
    {
        const cce_ds_leaf* kv =
            cce_ds_map_find_role(&map, CCE_DS_ROLE_MLA_KV_DOWN, 0, -1);
        check(kv && kv->dim_in == hp.d_model &&
              kv->dim_out == hp.kv_lora_rank + hp.qk_rope_head_dim,
              "MLA kv_down dim contract");
    }

    /* MLA config bridge */
    {
        cce_mla_config mla;
        cce_ds_hparams_to_mla(&hp, &mla);
        check(mla.kv_lora_rank == hp.kv_lora_rank &&
              mla.d_model == hp.d_model, "hparams → mla config");
        check(cce_mla_cache_bytes_per_token(&mla) <
              cce_mha_cache_bytes_per_token(hp.n_heads,
                  hp.qk_nope_head_dim + hp.qk_rope_head_dim),
              "MLA cache << MHA via map dims");
    }

    /* Bind check: missing experts → not found */
    {
        cce_ds_bind_report r;
        cce_result rc = cce_ds_map_bind_check(&map, fake_has, NULL, &r);
        check(rc != CCE_OK, "bind fails when experts missing");
        check(r.missing_required > 0 && r.present > 0, "bind report counts");
        printf("    bind: present=%d miss_req=%d miss_opt=%d first_miss=%d\n",
               r.present, r.missing_required, r.missing_optional,
               r.first_missing_leaf);
    }

    /* V3-shaped map builds (no weight load — structure only) */
    {
        cce_ds_map big;
        cce_ds_hparams v3;
        cce_ds_hparams_default_v3(&v3);
        /* shrink experts for test time/memory of leaf table */
        v3.n_layer = 4;
        v3.n_expert = 16;
        check(cce_ds_map_build(&big, &v3) == CCE_OK, "build mid-scale map");
        check(cce_ds_map_validate(&big, err, sizeof err) == CCE_OK,
              "validate mid-scale");
        printf("    mid-scale leaves=%d (4 layers × 16 experts)\n", big.n_leaves);
        cce_ds_map_free(&big);
    }

    /* Role names stable */
    check(strcmp(cce_ds_role_name(CCE_DS_ROLE_MLA_KV_DOWN), "mla.kv_down") == 0,
          "role_name mla.kv_down");

    cce_ds_map_free(&map);

    printf("DEEPSEEK_MAP_PASS checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
