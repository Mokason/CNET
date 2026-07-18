/* CNET-native DeepSeek forest tensor map — make deepseek_map → DEEPSEEK_MAP_PASS */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_deepseek_map.h"
#include "../include/cce/cce_mla.h"
#include "../include/cce/cce_forest.h"
#include "../include/cce/cce_cascade.h"
#include "../include/cce/cce_tensor.h"

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

    /* ---- Real cce_forest bind (synthetic, no GGUF / no DeepSeek project) ---- */
    {
        cce_forest* forest = NULL;
        cce_ds_bind_opts opts;
        cce_ds_bind_result br;
        cce_ds_bind_opts_default(&opts, "ds_bind_test.cce");
        opts.synthetic = 1;
        opts.bind_cold = 0; /* HOT (+WARM) only — isolation from expert banks */
        opts.wire_connections = 1;
        check(cce_ds_map_bind_forest(&map, &forest, &opts, &br) == CCE_OK,
              "bind_forest synthetic");
        check(forest != NULL && forest->num_branches > 0, "forest has branches");
        check(br.bound > 0 && br.failed == 0, "bound > 0, failed == 0");
        printf("    bind: bound=%d skip_norm=%d skip_cold=%d\n",
               br.bound, br.skipped_norm, br.skipped_cold);

        check(cce_forest_get_resident(forest, "trunk.embed") != NULL,
              "resident trunk.embed");
        check(cce_forest_get_resident(forest, "L00.mla.kv_dn") != NULL,
              "resident L00.mla.kv_dn");
        check(cce_forest_get_resident(forest, "L00.ffn.route") != NULL,
              "resident L00.ffn.route");
        /* COLD experts not bound by default */
        check(cce_forest_get_resident(forest, "L00.ffn.e000.g") == NULL,
              "cold expert not bound (demand-load later)");

        /* Structure connection kv_up specializes kv_dn */
        {
            int b, found = 0;
            for (b = 0; b < forest->num_branches; ++b) {
                if (strcmp(forest->branches[b].name, "L00.mla.kv_up") == 0) {
                    int c;
                    for (c = 0; c < forest->branches[b].num_connections; ++c)
                        if (strstr(forest->branches[b].conn_names[c], "kv_dn"))
                            found = 1;
                }
            }
            check(found, "kv_up specializes kv_dn connection");
        }

        /* Forward through a bound leaf cascade (real weights, real forest) */
        {
            cce_cascade* cas = cce_forest_get_resident(forest, "L00.mla.kv_dn");
            cce_tensor in = {0}, out = {0};
            int ok = 0;
            if (cas && cas->num_blocks > 0) {
                int in_d = cas->blocks[0].weights.shape[0];
                int out_d = cas->blocks[0].weights.shape[1];
                int sh[1] = { in_d };
                cce_tensor_alloc(&in, sh, 1);
                {
                    int j;
                    for (j = 0; j < in_d; ++j) in.data[j] = 0.01f * (float)j;
                }
                if (cce_cascade_forward(cas, &in, &out) == CCE_OK &&
                    out.data && out.numel == (size_t)out_d) {
                    int j, finite = 1;
                    double amax = 0;
                    for (j = 0; j < out_d; ++j) {
                        if (!isfinite(out.data[j])) finite = 0;
                        if (fabs((double)out.data[j]) > amax)
                            amax = fabs((double)out.data[j]);
                    }
                    ok = finite && amax > 0;
                }
                cce_tensor_free(&in);
                cce_tensor_free(&out);
            }
            check(ok, "cascade_forward on bound MLA leaf");
        }

        /* CNET pack round-trip (native, not GGUF) */
        {
            cce_ds_pack* pack = NULL;
            cce_forest* f2 = NULL;
            cce_ds_bind_opts o2;
            cce_ds_bind_result br2;
            check(cce_ds_pack_write("ds_test.cnetpack", &map, NULL, NULL) == CCE_OK,
                  "cnetpack write (synthetic fill)");
            check(cce_ds_pack_open(&pack, "ds_test.cnetpack") == CCE_OK,
                  "cnetpack open");
            cce_ds_bind_opts_default(&o2, "ds_bind_pack.cce");
            o2.synthetic = 0;
            o2.load_weight = cce_ds_pack_load_weight;
            o2.load_ctx = pack;
            o2.bind_cold = 0;
            check(cce_ds_map_bind_forest(&map, &f2, &o2, &br2) == CCE_OK,
                  "bind_forest from cnetpack");
            check(br2.bound > 0 && cce_forest_get_resident(f2, "trunk.head") != NULL,
                  "pack-bound trunk.head resident");
            cce_ds_pack_close(pack);
            if (f2) cce_forest_close(f2);
            remove("ds_test.cnetpack");
            remove("ds_bind_pack.cce");
        }

        if (forest) cce_forest_close(forest);
        remove("ds_bind_test.cce");
    }

    check(cce_ds_role_is_linear(CCE_DS_ROLE_MLA_KV_DOWN) &&
          !cce_ds_role_is_linear(CCE_DS_ROLE_ATTN_NORM),
          "role_is_linear distinguishes norms");

    cce_ds_map_free(&map);

    printf("DEEPSEEK_MAP_PASS checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
