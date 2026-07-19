/* Contract-graph topology audit -- see include/topology.h.
 * Read-only. Betti-0 via union-find, Betti-1 via cycle rank (E - V + C). */
#include "../include/topology.h"
#include "../include/cnet_platform.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

const char *topology_port_sig(Port p, char *buf, size_t n) {
    if (!buf || n == 0) return buf;
    if (p.tag[0])
        snprintf(buf, n, "F%dw%zuc%zu/%s", (int)p.family, p.field_width, p.field_count, p.tag);
    else
        snprintf(buf, n, "F%dw%zuc%zu", (int)p.family, p.field_width, p.field_count);
    return buf;
}

/* ---- union-find ---- */
static int uf_find(int *parent, int x) {
    while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
}
static void uf_union(int *parent, int a, int b) {
    int ra = uf_find(parent, a), rb = uf_find(parent, b);
    if (ra != rb) parent[ra] = rb;
}

/* Can producer p feed consumer q? (some output port of p compatible with some
   input port of q). */
static int can_feed(const BinaryTransformNetwork *p, const BinaryTransformNetwork *q) {
    for (size_t oi = 0; oi < p->output_port_count; oi++)
        for (size_t ii = 0; ii < q->input_port_count; ii++)
            if (port_compatible(p->output_ports[oi], q->input_ports[ii])) return 1;
    return 0;
}

/* Does any eligible node consume type `in_sig` AND produce type `out_sig`?
   (i.e. does the bridge in_sig->out_sig already exist) */
static int transform_exists(const BinaryTransformNetwork **btn, size_t v,
                            const char *in_sig, const char *out_sig) {
    char s[TOPO_SIG_MAX];
    for (size_t k = 0; k < v; k++) {
        int has_in = 0, has_out = 0;
        for (size_t ii = 0; ii < btn[k]->input_port_count; ii++)
            if (strcmp(topology_port_sig(btn[k]->input_ports[ii], s, sizeof s), in_sig) == 0) { has_in = 1; break; }
        if (!has_in) continue;
        for (size_t oi = 0; oi < btn[k]->output_port_count; oi++)
            if (strcmp(topology_port_sig(btn[k]->output_ports[oi], s, sizeof s), out_sig) == 0) { has_out = 1; break; }
        if (has_in && has_out) return 1;
    }
    return 0;
}

/* full positional signature of a node, for dedup detection */
static void node_full_sig(const BinaryTransformNetwork *b, char *out, size_t n) {
    char s[TOPO_SIG_MAX]; size_t w = 0;
    w += (size_t)snprintf(out + w, w < n ? n - w : 0, "IN:");
    for (size_t i = 0; i < b->input_port_count; i++)
        w += (size_t)snprintf(out + w, w < n ? n - w : 0, "%s|", topology_port_sig(b->input_ports[i], s, sizeof s));
    w += (size_t)snprintf(out + w, w < n ? n - w : 0, ";OUT:");
    for (size_t i = 0; i < b->output_port_count; i++)
        w += (size_t)snprintf(out + w, w < n ? n - w : 0, "%s|", topology_port_sig(b->output_ports[i], s, sizeof s));
}

static void add_suggestion(TopoReport *r, const char *consume_sig, const char *produce_sig,
                           int from_c, int to_c, size_t score) {
    /* dedup by (consume,produce): keep the higher score */
    for (size_t i = 0; i < r->suggestion_count; i++) {
        if (strcmp(r->suggestions[i].consume_sig, consume_sig) == 0 &&
            strcmp(r->suggestions[i].produce_sig, produce_sig) == 0) {
            if (score > r->suggestions[i].score) {
                r->suggestions[i].score = score;
                r->suggestions[i].from_component = from_c;
                r->suggestions[i].to_component = to_c;
            }
            return;
        }
    }
    if (r->suggestion_count >= TOPO_MAX_SUGGESTIONS) return;
    TopoBridgeSuggestion *s = &r->suggestions[r->suggestion_count++];
    snprintf(s->consume_sig, sizeof s->consume_sig, "%s", consume_sig);
    snprintf(s->produce_sig, sizeof s->produce_sig, "%s", produce_sig);
    s->from_component = from_c;
    s->to_component = to_c;
    s->score = score;
}

int topology_analyze(const PrimitiveRegistry *reg, int certified_only, TopoReport *out) {
    if (!reg || !out) return -1;
    memset(out, 0, sizeof *out);

    /* collect eligible nodes */
    const BinaryTransformNetwork *btn[TOPO_MAX_NODES];
    const char *names[TOPO_MAX_NODES];
    size_t v = 0;
    for (size_t i = 0; i < reg->count; i++) {
        if (certified_only && !reg->entries[i].certified) continue;
        if (!reg->entries[i].btn) continue;
        if (v >= TOPO_MAX_NODES) return -1;
        btn[v] = reg->entries[i].btn;
        names[v] = reg->entries[i].name;
        v++;
    }
    out->node_count = v;

    int parent[TOPO_MAX_NODES];
    for (size_t i = 0; i < v; i++) parent[i] = (int)i;

    /* edges (undirected, no self-loops) */
    size_t edges = 0;
    for (size_t i = 0; i < v; i++)
        for (size_t j = i + 1; j < v; j++)
            if (can_feed(btn[i], btn[j]) || can_feed(btn[j], btn[i])) {
                edges++;
                uf_union(parent, (int)i, (int)j);
            }
    out->edge_count = edges;

    /* component ids 0..C-1 in first-appearance order */
    int comp_id[TOPO_MAX_NODES];
    int root_to_comp[TOPO_MAX_NODES];
    for (size_t i = 0; i < v; i++) root_to_comp[i] = -1;
    size_t ncomp = 0;
    for (size_t i = 0; i < v; i++) {
        int r = uf_find(parent, (int)i);
        if (root_to_comp[r] < 0) root_to_comp[r] = (int)ncomp++;
        comp_id[i] = root_to_comp[r];
        out->node_name[i] = names[i];
        out->node_component[i] = comp_id[i];
    }
    out->betti0 = ncomp;
    out->betti1 = (long)edges - (long)v + (long)ncomp;  /* cycle rank */

    /* component sizes */
    size_t comp_size[TOPO_MAX_NODES];
    for (size_t c = 0; c < ncomp; c++) comp_size[c] = 0;
    for (size_t i = 0; i < v; i++) comp_size[comp_id[i]]++;
    for (size_t c = 0; c < ncomp; c++)
        if (comp_size[c] > out->largest_component_size) out->largest_component_size = comp_size[c];

    /* "what to mint next": bridges that would merge two components.
       To join Ca (produces P) -> Cb (consumes Q), mint a primitive P->Q. */
    char ps[TOPO_SIG_MAX], qs[TOPO_SIG_MAX];
    for (size_t i = 0; i < v && out->suggestion_count < TOPO_MAX_SUGGESTIONS; i++) {
        int ca = comp_id[i];
        for (size_t oi = 0; oi < btn[i]->output_port_count; oi++) {
            topology_port_sig(btn[i]->output_ports[oi], ps, sizeof ps);   /* P produced by Ca */
            for (size_t j = 0; j < v; j++) {
                int cb = comp_id[j];
                if (cb == ca) continue;
                for (size_t ji = 0; ji < btn[j]->input_port_count; ji++) {
                    topology_port_sig(btn[j]->input_ports[ji], qs, sizeof qs);  /* Q consumed by Cb */
                    if (strcmp(ps, qs) == 0) continue;          /* same type -> already joinable */
                    if (transform_exists(btn, v, ps, qs)) continue;
                    add_suggestion(out, ps, qs, ca, cb, comp_size[ca] + comp_size[cb]);
                }
            }
        }
    }

    /* dedup candidates: identical full positional signatures */
    char sigi[1024], sigj[1024];
    for (size_t i = 0; i < v && out->dedup_count < TOPO_MAX_DEDUP; i++) {
        node_full_sig(btn[i], sigi, sizeof sigi);
        for (size_t j = i + 1; j < v && out->dedup_count < TOPO_MAX_DEDUP; j++) {
            node_full_sig(btn[j], sigj, sizeof sigj);
            if (strcmp(sigi, sigj) == 0) {
                TopoDedupCandidate *d = &out->dedup[out->dedup_count++];
                snprintf(d->name_a, sizeof d->name_a, "%s", names[i] ? names[i] : "?");
                snprintf(d->name_b, sizeof d->name_b, "%s", names[j] ? names[j] : "?");
            }
        }
    }

    /* sort suggestions by score desc (small n; insertion sort) */
    for (size_t i = 1; i < out->suggestion_count; i++) {
        TopoBridgeSuggestion key = out->suggestions[i];
        size_t k = i;
        while (k > 0 && out->suggestions[k - 1].score < key.score) {
            out->suggestions[k] = out->suggestions[k - 1]; k--;
        }
        out->suggestions[k] = key;
    }
    return 0;
}

static void ensure_parent_dirs(const char *path) {
    char buf[512]; size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) return;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++)
        if (buf[i] == '/' || buf[i] == '\\') {
            char c = buf[i]; buf[i] = '\0';
            cnet_mkdir(buf, 0755);                 /* best-effort; EEXIST is fine */
            buf[i] = c;
        }
}

int topology_write_json(const TopoReport *r, const char *path) {
    if (!r || !path) return -1;
    ensure_parent_dirs(path);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "{\n");
    fprintf(f, "  \"betti0\": %zu,\n", r->betti0);
    fprintf(f, "  \"betti1\": %ld,\n", r->betti1);
    fprintf(f, "  \"node_count\": %zu,\n", r->node_count);
    fprintf(f, "  \"edge_count\": %zu,\n", r->edge_count);
    fprintf(f, "  \"largest_component\": %zu,\n", r->largest_component_size);

    fprintf(f, "  \"components\": [\n");
    for (size_t c = 0; c < r->betti0; c++) {
        fprintf(f, "    { \"id\": %zu, \"members\": [", c);
        int first = 1;
        for (size_t i = 0; i < r->node_count; i++)
            if (r->node_component[i] == (int)c) {
                fprintf(f, "%s\"%s\"", first ? "" : ", ", r->node_name[i] ? r->node_name[i] : "?");
                first = 0;
            }
        fprintf(f, "] }%s\n", c + 1 < r->betti0 ? "," : "");
    }
    fprintf(f, "  ],\n");

    fprintf(f, "  \"mint_candidates\": [\n");
    for (size_t i = 0; i < r->suggestion_count; i++)
        fprintf(f, "    { \"consume\": \"%s\", \"produce\": \"%s\", \"from_component\": %d, "
                   "\"to_component\": %d, \"score\": %zu }%s\n",
                r->suggestions[i].consume_sig, r->suggestions[i].produce_sig,
                r->suggestions[i].from_component, r->suggestions[i].to_component,
                r->suggestions[i].score, i + 1 < r->suggestion_count ? "," : "");
    fprintf(f, "  ],\n");

    fprintf(f, "  \"dedup_candidates\": [\n");
    for (size_t i = 0; i < r->dedup_count; i++)
        fprintf(f, "    { \"a\": \"%s\", \"b\": \"%s\" }%s\n",
                r->dedup[i].name_a, r->dedup[i].name_b, i + 1 < r->dedup_count ? "," : "");
    fprintf(f, "  ],\n");

    fprintf(f, "  \"note\": \"mint_candidates and dedup_candidates are advisory observations, "
               "not automatic evolution\"\n");
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}

void topology_print_report(const TopoReport *r) {
    if (!r) return;
    printf("=== Contract-graph topology audit ===\n");
    printf("  nodes(V)=%zu  edges(E)=%zu  betti0(components)=%zu  betti1(cycle-rank)=%ld  largest-comp=%zu\n",
           r->node_count, r->edge_count, r->betti0, r->betti1, r->largest_component_size);
    if (r->betti0 > 1)
        printf("  -> %zu capability islands (cannot currently interoperate)\n", r->betti0);
    if (r->betti1 > 0)
        printf("  -> %ld redundant composition path(s) (dedup/consolidation may collapse)\n", r->betti1);

    printf("  components:\n");
    for (size_t c = 0; c < r->betti0; c++) {
        printf("    [%zu]", c);
        for (size_t i = 0; i < r->node_count; i++)
            if (r->node_component[i] == (int)c) printf(" %s", r->node_name[i] ? r->node_name[i] : "?");
        printf("\n");
    }

    if (r->suggestion_count) {
        printf("  what to mint next (bridges that reduce betti0):\n");
        for (size_t i = 0; i < r->suggestion_count; i++)
            printf("    mint  %s -> %s   (joins comp %d -> %d, score %zu)\n",
                   r->suggestions[i].consume_sig, r->suggestions[i].produce_sig,
                   r->suggestions[i].from_component, r->suggestions[i].to_component,
                   r->suggestions[i].score);
    }
    if (r->dedup_count) {
        printf("  dedup candidates (identical port signatures):\n");
        for (size_t i = 0; i < r->dedup_count; i++)
            printf("    %s == %s (by signature)\n", r->dedup[i].name_a, r->dedup[i].name_b);
    }
}
