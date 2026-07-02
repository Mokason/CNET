/* SIMILAR_TO + evidence-gated merge. See include/cce/cce_similar.h. */

#include "../../include/cce/cce_similar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int cce_similar_candidates(const cce_spec_graph* ga, const cce_spec_graph* gb,
                           float tau, cce_similar_pair* out, int cap) {
    if (!ga || !gb || !out || cap <= 0) return 0;
    int n = 0;
    for (int i = 0; i < ga->n_nodes && n < cap; i++) {
        const cce_spec_node* a = &ga->nodes[i];
        for (int j = 0; j < gb->n_nodes && n < cap; j++) {
            const cce_spec_node* b = &gb->nodes[j];
            if (a->digest == b->digest) continue;            /* exact dup: store handles it */
            if (a->in_dim != b->in_dim || a->out_dim != b->out_dim) continue;
            float d2 = 0;
            for (int s = 0; s < CCE_SPEC_SIG_DIM; s++) {
                float d = a->sig[s] - b->sig[s];
                d2 += d * d;
            }
            float dist = sqrtf(d2);
            if (dist > tau) continue;
            cce_similar_pair* p = &out[n++];
            memset(p, 0, sizeof(*p));
            strncpy(p->name_a, a->name, sizeof(p->name_a) - 1);
            strncpy(p->name_b, b->name, sizeof(p->name_b) - 1);
            p->digest_a = a->digest;
            p->digest_b = b->digest;
            p->sig_dist = dist;
        }
    }
    return n;
}

cce_result cce_similar_verify(cce_weight_store* s, cce_similar_pair* p,
                              int probes, float epsilon) {
    if (!s || !p || probes < 1) return CCE_ERR_INVALID_ARG;
    p->max_rel_dev = -1.0f;
    p->probes = 0;
    p->equivalent = 0;

    cce_cascade *ca = NULL, *cb = NULL;
    cce_result rc = cce_weight_store_get(s, p->digest_a, &ca);
    if (rc == CCE_OK) rc = cce_weight_store_get(s, p->digest_b, &cb);
    if (rc != CCE_OK) {
        if (ca) cce_cascade_destroy(ca);
        if (cb) cce_cascade_destroy(cb);
        return rc;
    }

    int in_dim = (ca->num_blocks > 0 && ca->blocks[0].weights.ndim >= 2)
               ? ca->blocks[0].weights.shape[0] : 0;
    if (in_dim <= 0) { cce_cascade_destroy(ca); cce_cascade_destroy(cb); return CCE_ERR_UNSUPPORTED; }

    cce_tensor tin = {0};
    int ish[1] = { in_dim };
    if (cce_tensor_alloc(&tin, ish, 1) != CCE_OK) {
        cce_cascade_destroy(ca); cce_cascade_destroy(cb); return CCE_ERR_OOM;
    }

    float worst = 0.0f;
    /* battery seeds are distinct from the fingerprint seeds on purpose:
       equivalence must hold on inputs the signature never saw */
    for (int k = 0; k < probes && rc == CCE_OK; k++) {
        uint64_t seed = 0xBADC0DEULL + (uint64_t)k * 0xD1B54A32D192ED03ULL;
        for (int i = 0; i < in_dim; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            tin.data[i] = (float)((double)(seed >> 33) / 2147483648.0 - 1.0);
        }
        cce_tensor oa = {0}, ob = {0};
        rc = cce_cascade_forward(ca, &tin, &oa);
        if (rc == CCE_OK) rc = cce_cascade_forward(cb, &tin, &ob);
        if (rc == CCE_OK) {
            if (oa.numel != ob.numel) rc = CCE_ERR_UNSUPPORTED;
            else {
                for (size_t v = 0; v < oa.numel; v++) {
                    float ma = fabsf(oa.data[v]), mb = fabsf(ob.data[v]);
                    float denom = 1e-6f + (ma > mb ? ma : mb);
                    float dev = fabsf(oa.data[v] - ob.data[v]) / denom;
                    if (dev > worst) worst = dev;
                }
                p->probes++;
            }
        }
        cce_tensor_free(&oa);
        cce_tensor_free(&ob);
    }
    cce_tensor_free(&tin);
    cce_cascade_destroy(ca);
    cce_cascade_destroy(cb);
    if (rc != CCE_OK) return rc;

    p->max_rel_dev = worst;
    p->equivalent = (worst <= epsilon) ? 1 : 0;
    return CCE_OK;
}

cce_result cce_similar_merge(cce_weight_store* s, const cce_similar_pair* p,
                             const char* manifest_in, const char* manifest_out,
                             int* rows_rewritten) {
    if (rows_rewritten) *rows_rewritten = 0;
    if (!s || !p || !manifest_in || !manifest_out) return CCE_ERR_INVALID_ARG;
    /* the evidence gate: no verified battery, no merge */
    if (!p->equivalent || p->probes < 8) return CCE_ERR_UNSUPPORTED;
    if (!cce_weight_store_contains(s, p->digest_a)) return CCE_ERR_NOT_FOUND;

    FILE* in = fopen(manifest_in, "rb");
    if (!in) return CCE_ERR_IO;
    FILE* out = fopen(manifest_out, "wb");
    if (!out) { fclose(in); return CCE_ERR_IO; }

    char from_hex[32], to_hex[32];
    snprintf(from_hex, sizeof(from_hex), "%016llx", (unsigned long long)p->digest_b);
    snprintf(to_hex, sizeof(to_hex), "%016llx", (unsigned long long)p->digest_a);

    char line[512];
    int rewritten = 0;
    while (fgets(line, sizeof(line), in)) {
        char* hit = strstr(line, from_hex);
        if (hit && (strncmp(line, "spec ", 5) == 0 || strncmp(line, "tensor ", 7) == 0)) {
            memcpy(hit, to_hex, 16);
            rewritten++;
        }
        fputs(line, out);
    }
    fclose(in);
    fclose(out);
    if (rows_rewritten) *rows_rewritten = rewritten;
    return CCE_OK;
}
