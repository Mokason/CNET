/* Tiered runtime for store-backed transformers. See cce_tier_runtime.h. */

#include "../../include/cce/cce_tier_runtime.h"
#include "../../include/cce/cce_forest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char name[96]; uint64_t digest; } tier_entry;

struct cce_tier_runtime {
    cce_weight_store* store;
    cce_forest* forest;
    tier_entry* map;
    int n_map;
    int rehydrations;
};

static uint64_t tier_lookup(const cce_tier_runtime* rt, const char* name) {
    for (int i = 0; i < rt->n_map; i++)
        if (strcmp(rt->map[i].name, name) == 0) return rt->map[i].digest;
    return 0;
}

static cce_cascade* tier_provider(void* ctx, const char* branch_name) {
    cce_tier_runtime* rt = (cce_tier_runtime*)ctx;
    uint64_t d = tier_lookup(rt, branch_name);
    if (!d) return NULL;
    cce_cascade* cas = NULL;
    if (cce_weight_store_get(rt->store, d, &cas) != CCE_OK) return NULL;
    rt->rehydrations++;
    return cas; /* forest adopts */
}

cce_result cce_tier_attach(cce_tier_runtime** out, cce_gguf_qwen2* m,
                           cce_weight_store* s, const char* manifest_path,
                           int hot_cap) {
    if (!out || !m || !m->forest || !s || !manifest_path) return CCE_ERR_INVALID_ARG;
    *out = NULL;

    FILE* f = fopen(manifest_path, "rb");
    if (!f) return CCE_ERR_IO;
    char line[512];
    if (!fgets(line, sizeof(line), f) || strncmp(line, "CNET_MANIFEST v1", 16) != 0) {
        fclose(f); return CCE_ERR_UNSUPPORTED;
    }

    cce_tier_runtime* rt = (cce_tier_runtime*)calloc(1, sizeof(*rt));
    if (!rt) { fclose(f); return CCE_ERR_OOM; }
    rt->store = s;
    rt->forest = m->forest;
    rt->map = (tier_entry*)calloc((size_t)m->forest->num_branches, sizeof(tier_entry));
    if (!rt->map) { free(rt); fclose(f); return CCE_ERR_OOM; }

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "spec ", 5) != 0) continue;
        if (rt->n_map >= m->forest->num_branches) break;
        char name[96];
        unsigned long long d = 0;
        if (sscanf(line + 5, "%95s %llx", name, &d) == 2) {
            strncpy(rt->map[rt->n_map].name, name, sizeof(rt->map[0].name) - 1);
            rt->map[rt->n_map].digest = (uint64_t)d;
            rt->n_map++;
        }
    }
    fclose(f);

    /* every forest branch must be restorable, and its payload present,
       before anything becomes evictable */
    for (int i = 0; i < m->forest->num_branches; i++) {
        uint64_t d = tier_lookup(rt, m->forest->branches[i].name);
        if (!d || !cce_weight_store_contains(s, d)) {
            free(rt->map); free(rt);
            return CCE_ERR_UNSUPPORTED;
        }
    }
    for (int i = 0; i < m->forest->num_branches; i++)
        m->forest->branches[i].evictable = 1;

    cce_forest_set_residency(m->forest, hot_cap, tier_provider, rt);
    *out = rt;
    return CCE_OK;
}

cce_result cce_tier_evict_all(cce_tier_runtime* rt) {
    if (!rt || !rt->forest) return CCE_ERR_INVALID_ARG;
    for (int i = 0; i < rt->forest->num_branches; i++) {
        cce_branch* b = &rt->forest->branches[i];
        if (b->evictable && b->cascade && !b->is_view)
            cce_forest_evict_branch(rt->forest, i);
    }
    return CCE_OK;
}

int cce_tier_rehydrations(const cce_tier_runtime* rt) { return rt ? rt->rehydrations : 0; }
int cce_tier_resident(const cce_tier_runtime* rt) {
    return rt ? cce_forest_resident_count(rt->forest) : 0;
}
int cce_tier_high_water(const cce_tier_runtime* rt) {
    return rt ? cce_forest_resident_high_water(rt->forest) : 0;
}

cce_result cce_tier_detach(cce_tier_runtime* rt) {
    if (!rt || !rt->forest) return CCE_ERR_INVALID_ARG;
    /* rehydrate everything first so the model keeps working standalone;
       lift the cap so nothing gets evicted while restoring */
    rt->forest->hot_cap = rt->forest->num_branches + 1;
    for (int i = 0; i < rt->forest->num_branches; i++) {
        if (!rt->forest->branches[i].cascade) {
            if (!cce_forest_get_resident(rt->forest, rt->forest->branches[i].name)) {
                return CCE_ERR_IO; /* store lost a payload: refuse to detach */
            }
        }
    }
    cce_forest_set_residency(rt->forest, 0, NULL, NULL); /* clears evictable flags */
    free(rt->map);
    free(rt);
    return CCE_OK;
}
