/* Specialist knowledge graph. See include/cce/cce_specgraph.h. */

#include "../../include/cce/cce_specgraph.h"
#include "../../include/cce/cce_forest.h"
#include "../../include/cce/cce_block.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- identity: FNV-1a 64 over structure + parameter bytes ---- */

#define FNV_OFFSET 1469598103934665603ULL
#define FNV_PRIME  1099511628211ULL

static uint64_t fnv1a(uint64_t h, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= FNV_PRIME; }
    return h;
}

uint64_t cce_spec_digest(const cce_cascade* cas) {
    if (!cas) return 0;
    uint64_t h = FNV_OFFSET;
    int nb = cas->num_blocks;
    h = fnv1a(h, &nb, sizeof(nb));
    for (int b = 0; b < nb; b++) {
        const cce_block* blk = &cas->blocks[b];
        int t = (int)blk->type;
        int in  = (blk->weights.ndim >= 2) ? blk->weights.shape[0] : 0;
        int out = (blk->weights.ndim >= 2) ? blk->weights.shape[1] : 0;
        int has_bias = (blk->bias.data && blk->bias.numel > 0) ? 1 : 0;
        h = fnv1a(h, &t, sizeof(t));
        h = fnv1a(h, &in, sizeof(in));
        h = fnv1a(h, &out, sizeof(out));
        h = fnv1a(h, &has_bias, sizeof(has_bias));
        if (blk->weights.data && blk->weights.numel > 0)
            h = fnv1a(h, blk->weights.data, blk->weights.numel * sizeof(float));
        if (has_bias)
            h = fnv1a(h, blk->bias.data, blk->bias.numel * sizeof(float));
    }
    return h;
}

/* K deterministic probes: same weights => same fingerprint, bit for bit. */
#define SPEC_PROBES 4

uint64_t cce_spec_fingerprint(const cce_cascade* cas, float sig_out[CCE_SPEC_SIG_DIM]) {
    if (sig_out) memset(sig_out, 0, CCE_SPEC_SIG_DIM * sizeof(float));
    if (!cas || cas->num_blocks < 1) return 0;
    int in_dim = (cas->blocks[0].weights.ndim >= 2) ? cas->blocks[0].weights.shape[0] : 0;
    if (in_dim <= 0) return 0;

    uint64_t h = FNV_OFFSET;
    cce_tensor tin = {0};
    int ish[1] = { in_dim };
    if (cce_tensor_alloc(&tin, ish, 1) != CCE_OK) return 0;

    for (int p = 0; p < SPEC_PROBES; p++) {
        uint64_t seed = 0xC0FFEEULL + (uint64_t)p * 0x9E3779B97F4A7C15ULL;
        for (int i = 0; i < in_dim; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            tin.data[i] = (float)((double)(seed >> 33) / 2147483648.0 - 1.0);
        }
        cce_tensor tout = {0};
        if (cce_cascade_forward(cas, &tin, &tout) != CCE_OK) {
            cce_tensor_free(&tout);
            cce_tensor_free(&tin);
            return 0;
        }
        h = fnv1a(h, tout.data, tout.numel * sizeof(float));
        if (p == 0 && sig_out) {
            for (int i = 0; i < CCE_SPEC_SIG_DIM && (size_t)i < tout.numel; i++)
                sig_out[i] = tout.data[i];
        }
        cce_tensor_free(&tout);
    }
    cce_tensor_free(&tin);
    return h;
}

/* ---- graph assembly ---- */

static void role_from_name(const char* name, char* role, size_t cap) {
    const char* last = name;
    for (const char* p = name; *p; p++) if (*p == '.') last = p + 1;
    strncpy(role, last, cap - 1);
    role[cap - 1] = 0;
}

static cce_result graph_alloc(cce_spec_graph** out, const char* model_name,
                              int max_nodes, int max_edges) {
    cce_spec_graph* g = (cce_spec_graph*)calloc(1, sizeof(*g));
    if (!g) return CCE_ERR_OOM;
    g->nodes = (cce_spec_node*)calloc((size_t)max_nodes, sizeof(cce_spec_node));
    g->edges = (cce_spec_edge*)calloc((size_t)(max_edges > 0 ? max_edges : 1), sizeof(cce_spec_edge));
    if (!g->nodes || !g->edges) { cce_spec_graph_free(g); return CCE_ERR_OOM; }
    if (model_name) strncpy(g->model_name, model_name, sizeof(g->model_name) - 1);
    return (*out = g), CCE_OK;
}

static void add_node(cce_spec_graph* g, const char* name, cce_cascade* cas) {
    cce_spec_node* n = &g->nodes[g->n_nodes++];
    strncpy(n->name, name, sizeof(n->name) - 1);
    role_from_name(name, n->role, sizeof(n->role));
    n->n_blocks = cas ? cas->num_blocks : 0;
    if (cas && cas->num_blocks > 0) {
        n->in_dim  = cas->blocks[0].weights.ndim >= 2 ? cas->blocks[0].weights.shape[0] : 0;
        const cce_block* lastb = &cas->blocks[cas->num_blocks - 1];
        n->out_dim = lastb->weights.ndim >= 2 ? lastb->weights.shape[1] : 0;
    }
    n->digest = cce_spec_digest(cas);
    n->fingerprint = cce_spec_fingerprint(cas, n->sig);
}

static void add_edge(cce_spec_graph* g, int src, int dst, const char* kind) {
    if (src < 0 || dst < 0) return;
    cce_spec_edge* e = &g->edges[g->n_edges++];
    e->src = src; e->dst = dst;
    strncpy(e->kind, kind, sizeof(e->kind) - 1);
}

int cce_spec_graph_find(const cce_spec_graph* g, const char* name) {
    if (!g || !name) return -1;
    for (int i = 0; i < g->n_nodes; i++)
        if (strcmp(g->nodes[i].name, name) == 0) return i;
    return -1;
}

cce_result cce_spec_graph_from_forest(cce_spec_graph** out, cce_forest* f,
                                      const char* model_name) {
    if (!out || !f) return CCE_ERR_INVALID_ARG;
    *out = NULL;
    cce_spec_graph* g = NULL;
    cce_result rc = graph_alloc(&g, model_name, f->num_branches, 0);
    if (rc != CCE_OK) return rc;
    for (int b = 0; b < f->num_branches; b++)
        add_node(g, f->branches[b].name, f->branches[b].cascade);
    *out = g;
    return CCE_OK;
}

/* transformer wiring per layer: q,k,v feed the attention glue -> o;
 * o (through residual+norm) feeds gate and up; gate,up -> down;
 * down -> next layer's q/k/v; last down -> lm_head. */
static void wire_transformer(cce_spec_graph* g, int n_layer) {
    char a[96], b[96];
    for (int l = 0; l < n_layer; l++) {
#define IDX(fmt, ...) (snprintf(a, sizeof(a), fmt, __VA_ARGS__), cce_spec_graph_find(g, a))
        int q = IDX("qwen2.blk.%d.q_proj", l);
        int k = IDX("qwen2.blk.%d.k_proj", l);
        int v = IDX("qwen2.blk.%d.v_proj", l);
        int o = IDX("qwen2.blk.%d.o_proj", l);
        int gt = IDX("qwen2.blk.%d.gate_proj", l);
        int up = IDX("qwen2.blk.%d.up_proj", l);
        int dn = IDX("qwen2.blk.%d.down_proj", l);
        add_edge(g, q, o, "DATA_FLOWS");
        add_edge(g, k, o, "DATA_FLOWS");
        add_edge(g, v, o, "DATA_FLOWS");
        add_edge(g, o, gt, "DATA_FLOWS");
        add_edge(g, o, up, "DATA_FLOWS");
        add_edge(g, gt, dn, "DATA_FLOWS");
        add_edge(g, up, dn, "DATA_FLOWS");
        if (l + 1 < n_layer) {
            snprintf(b, sizeof(b), "qwen2.blk.%d.down_proj", l);
            int dsrc = cce_spec_graph_find(g, b);
            snprintf(b, sizeof(b), "qwen2.blk.%d.q_proj", l + 1);
            add_edge(g, dsrc, cce_spec_graph_find(g, b), "DATA_FLOWS");
            snprintf(b, sizeof(b), "qwen2.blk.%d.k_proj", l + 1);
            add_edge(g, dsrc, cce_spec_graph_find(g, b), "DATA_FLOWS");
            snprintf(b, sizeof(b), "qwen2.blk.%d.v_proj", l + 1);
            add_edge(g, dsrc, cce_spec_graph_find(g, b), "DATA_FLOWS");
        } else {
            add_edge(g, dn, cce_spec_graph_find(g, "qwen2.lm_head"), "DATA_FLOWS");
        }
#undef IDX
    }
}

/* ssm wiring per layer: in_proj -> x_proj -> dt_proj; in_proj (z gate) and
 * x_proj (B,C) and dt_proj (dt) all feed the scan whose output -> out_proj;
 * out_proj -> next in_proj; last out_proj -> lm_head. */
static void wire_ssm(cce_spec_graph* g, int n_layer) {
    char a[96], b[96];
    for (int l = 0; l < n_layer; l++) {
#define IDX(fmt, ...) (snprintf(a, sizeof(a), fmt, __VA_ARGS__), cce_spec_graph_find(g, a))
        int in = IDX("mamba.blk.%d.in_proj", l);
        int x  = IDX("mamba.blk.%d.x_proj", l);
        int dt = IDX("mamba.blk.%d.dt_proj", l);
        int ot = IDX("mamba.blk.%d.out_proj", l);
        add_edge(g, in, x, "DATA_FLOWS");
        add_edge(g, x, dt, "DATA_FLOWS");
        add_edge(g, in, ot, "DATA_FLOWS");
        add_edge(g, x, ot, "DATA_FLOWS");
        add_edge(g, dt, ot, "DATA_FLOWS");
        if (l + 1 < n_layer) {
            snprintf(b, sizeof(b), "mamba.blk.%d.in_proj", l + 1);
            add_edge(g, ot, cce_spec_graph_find(g, b), "DATA_FLOWS");
        } else {
            add_edge(g, ot, cce_spec_graph_find(g, "mamba.lm_head"), "DATA_FLOWS");
        }
#undef IDX
    }
}

cce_result cce_spec_graph_build(cce_spec_graph** out, const cce_anymodel* m,
                                const char* model_name) {
    if (!out || !m) return CCE_ERR_INVALID_ARG;
    *out = NULL;

    cce_forest* f = NULL;
    int n_layer = 0;
    int family = 0; /* 1 = transformer, 2 = ssm */
    if (m->transformer && m->transformer->forest) {
        f = m->transformer->forest;
        n_layer = m->transformer->n_layer;
        family = 1;
    } else if (m->ssm && m->ssm->forest) {
        f = m->ssm->forest;
        n_layer = m->ssm->n_layer;
        family = 2;
    } else if (m->forest) {
        f = m->forest;
    } else if (m->supra && m->supra->model && m->supra->model->forest) {
        f = m->supra->model->forest;
    }
    if (!f) return CCE_ERR_UNSUPPORTED;

    /* worst-case edges: 10 per layer + head */
    cce_spec_graph* g = NULL;
    cce_result rc = graph_alloc(&g, model_name, f->num_branches, 10 * (n_layer + 1) + 4);
    if (rc != CCE_OK) return rc;
    for (int b = 0; b < f->num_branches; b++)
        add_node(g, f->branches[b].name, f->branches[b].cascade);

    if (family == 1) wire_transformer(g, n_layer);
    else if (family == 2) wire_ssm(g, n_layer);

    *out = g;
    return CCE_OK;
}

void cce_spec_graph_free(cce_spec_graph* g) {
    if (!g) return;
    free(g->nodes);
    free(g->edges);
    free(g);
}

/* ---- flat-file persistence ---- */

cce_result cce_spec_graph_save(const cce_spec_graph* g, const char* path) {
    if (!g || !path) return CCE_ERR_INVALID_ARG;
    FILE* f = fopen(path, "wb");
    if (!f) return CCE_ERR_IO;
    fprintf(f, "CNET_SPECGRAPH v1\nmodel %s\n", g->model_name[0] ? g->model_name : "?");
    for (int i = 0; i < g->n_nodes; i++) {
        const cce_spec_node* n = &g->nodes[i];
        fprintf(f, "node %s %s %d %d %d %016llx %016llx",
                n->name, n->role[0] ? n->role : "?", n->in_dim, n->out_dim, n->n_blocks,
                (unsigned long long)n->digest, (unsigned long long)n->fingerprint);
        for (int s = 0; s < CCE_SPEC_SIG_DIM; s++) fprintf(f, " %.9g", (double)n->sig[s]);
        fprintf(f, "\n");
    }
    for (int i = 0; i < g->n_edges; i++)
        fprintf(f, "edge %d %d %s\n", g->edges[i].src, g->edges[i].dst, g->edges[i].kind);
    fprintf(f, "end\n");
    fclose(f);
    return CCE_OK;
}

cce_result cce_spec_graph_load(cce_spec_graph** out, const char* path) {
    if (!out || !path) return CCE_ERR_INVALID_ARG;
    *out = NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return CCE_ERR_IO;

    char line[512];
    if (!fgets(line, sizeof(line), f) || strncmp(line, "CNET_SPECGRAPH v1", 17) != 0) {
        fclose(f); return CCE_ERR_UNSUPPORTED;
    }

    /* first pass: count */
    long body = ftell(f);
    int nn = 0, ne = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "node ", 5) == 0) nn++;
        else if (strncmp(line, "edge ", 5) == 0) ne++;
    }
    fseek(f, body, SEEK_SET);

    cce_spec_graph* g = NULL;
    if (graph_alloc(&g, NULL, nn > 0 ? nn : 1, ne) != CCE_OK) { fclose(f); return CCE_ERR_OOM; }

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "model ", 6) == 0) {
            sscanf(line + 6, "%63s", g->model_name);
        } else if (strncmp(line, "node ", 5) == 0 && g->n_nodes < nn) {
            cce_spec_node* n = &g->nodes[g->n_nodes];
            unsigned long long dg = 0, fp = 0;
            double sg[CCE_SPEC_SIG_DIM] = {0};
            int got = sscanf(line + 5, "%95s %23s %d %d %d %llx %llx %lg %lg %lg %lg %lg %lg %lg %lg",
                             n->name, n->role, &n->in_dim, &n->out_dim, &n->n_blocks, &dg, &fp,
                             &sg[0], &sg[1], &sg[2], &sg[3], &sg[4], &sg[5], &sg[6], &sg[7]);
            if (got >= 7) {
                n->digest = dg; n->fingerprint = fp;
                for (int s = 0; s < CCE_SPEC_SIG_DIM; s++) n->sig[s] = (float)sg[s];
                g->n_nodes++;
            }
        } else if (strncmp(line, "edge ", 5) == 0 && g->n_edges < ne) {
            cce_spec_edge* e = &g->edges[g->n_edges];
            if (sscanf(line + 5, "%d %d %15s", &e->src, &e->dst, e->kind) == 3) g->n_edges++;
        }
    }
    fclose(f);
    *out = g;
    return CCE_OK;
}
