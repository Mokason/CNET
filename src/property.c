/*
 * Property: equational laws over primitives. A law names a typed source
 * signature and two chains; it holds iff both chains produce identical
 * canonical outputs for every member of the enumerated source domain.
 */
#include "../include/property.h"
#include "../include/plan_table.h"
#include "../include/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- local helpers (independent copies; mirrors contract.c by design) ---- */

static const char *family_token(PortFamily family) {
    switch (family) {
    case PORT_RAW:        return "raw";
    case PORT_ONEHOT:     return "onehot";
    case PORT_BINARY_MSB: return "binary_msb";
    case PORT_BINARY_LSB: return "binary_lsb";
    case PORT_EVIDENCE:   return "evidence";
    case PORT_CONCEPT:    return "concept";
    default:              return NULL;
    }
}

/* Returns the PortFamily for a token, or -1 cast to int on unknown. */
static int family_parse(const char *token) {
    if (strcmp(token, "raw") == 0)        return (int)PORT_RAW;
    if (strcmp(token, "onehot") == 0)     return (int)PORT_ONEHOT;
    if (strcmp(token, "binary_msb") == 0) return (int)PORT_BINARY_MSB;
    if (strcmp(token, "binary_lsb") == 0) return (int)PORT_BINARY_LSB;
    if (strcmp(token, "evidence") == 0)   return (int)PORT_EVIDENCE;
    if (strcmp(token, "concept") == 0)    return (int)PORT_CONCEPT;
    return -1;
}

/* A property name is a non-empty atom over [A-Za-z0-9_], length <
   PROPERTY_NAME_MAX. Returns 1 if valid, else 0. */
static int name_valid(const char *name) {
    size_t len;
    const char *p;
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    len = strlen(name);
    if (len >= PROPERTY_NAME_MAX) {
        return 0;
    }
    for (p = name; *p != '\0'; ++p) {
        char ch = *p;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_')) {
            return 0;
        }
    }
    return 1;
}

/* ---- property_save -------------------------------------------------------- */

int property_save(const Property *p, const char *path) {
    FILE *f;
    size_t i;

    if (p == NULL || path == NULL) {
        return -1;
    }
    if (!name_valid(p->name)) {
        return -1;
    }
    if (p->source_count == 0 || p->source_count > PROPERTY_MAX_SOURCES) {
        return -1;
    }
    if (p->lhs_len == 0 || p->lhs_len > PROPERTY_MAX_STEPS) {
        return -1;
    }
    if (p->rhs_len > PROPERTY_MAX_STEPS) {
        return -1;
    }

    /* Validate all chain atoms. */
    for (i = 0; i < p->lhs_len; ++i) {
        if (!name_valid(p->lhs[i])) {
            return -1;
        }
    }
    for (i = 0; i < p->rhs_len; ++i) {
        if (!name_valid(p->rhs[i])) {
            return -1;
        }
    }

    f = fopen(path, "w");
    if (f == NULL) {
        return -1;
    }

    if (fprintf(f, "CNET_PROPERTY 1\n") < 0) { goto fail; }
    if (fprintf(f, "%s\n", p->name) < 0) { goto fail; }
    if (fprintf(f, "SOURCES %lu\n", (unsigned long)p->source_count) < 0) {
        goto fail;
    }
    for (i = 0; i < p->source_count; ++i) {
        const Port *src = &p->sources[i];
        const char *tok = family_token(src->family);
        if (tok == NULL) { goto fail; }
        if (fprintf(f, "PORT %s %lu %lu %s\n",
                    tok,
                    (unsigned long)src->field_width,
                    (unsigned long)src->field_count,
                    src->tag[0] != '\0' ? src->tag : "-") < 0) {
            goto fail;
        }
    }

    if (fprintf(f, "LHS %lu\n", (unsigned long)p->lhs_len) < 0) { goto fail; }
    for (i = 0; i < p->lhs_len; ++i) {
        if (fprintf(f, "%s\n", p->lhs[i]) < 0) { goto fail; }
    }

    if (fprintf(f, "RHS %lu\n", (unsigned long)p->rhs_len) < 0) { goto fail; }
    for (i = 0; i < p->rhs_len; ++i) {
        if (fprintf(f, "%s\n", p->rhs[i]) < 0) { goto fail; }
    }

    /* fclose is the last I/O; close succeeds or we treat the file as corrupt.
       The file is already closed here so return directly -- do not goto fail,
       which would fclose a second time. */
    if (fclose(f) != 0) {
        return -1;
    }
    return 0;

fail:
    fclose(f);
    return -1;
}

/* ---- property_load -------------------------------------------------------- */

int property_load(Property *p, const char *path) {
    FILE *f;
    Property local;
    char magic[32];
    int version;
    char kw[16];
    unsigned long n_sources, n_lhs, n_rhs;
    char fam_tok[32];
    char tag_tok[PORT_TAG_MAX + 4]; /* room for the "-" sentinel */
    unsigned long field_width, field_count;
    size_t i;

    if (p == NULL || path == NULL) {
        return -1;
    }

    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }

    memset(&local, 0, sizeof local);

    /* magic + version */
    if (fscanf(f, "%31s %d", magic, &version) != 2) { goto fail; }
    if (strcmp(magic, "CNET_PROPERTY") != 0 || version != 1) { goto fail; }

    /* name: read as a token (whitespace-delimited), then validate as atom */
    if (fscanf(f, "%63s", local.name) != 1) { goto fail; }
    if (!name_valid(local.name)) { goto fail; }

    /* sources */
    if (fscanf(f, "%15s %lu", kw, &n_sources) != 2) { goto fail; }
    if (strcmp(kw, "SOURCES") != 0) { goto fail; }
    if (n_sources == 0 || n_sources > PROPERTY_MAX_SOURCES) { goto fail; }
    local.source_count = (size_t)n_sources;

    for (i = 0; i < local.source_count; ++i) {
        int fam;
        if (fscanf(f, "%15s %31s %lu %lu",
                   kw, fam_tok, &field_width, &field_count) != 4) {
            goto fail;
        }
        if (strcmp(kw, "PORT") != 0) { goto fail; }
        if (fscanf(f, "%35s", tag_tok) != 1) { goto fail; }
        fam = family_parse(fam_tok);
        if (fam < 0) { goto fail; }
        if (field_width == 0 || field_count == 0) { goto fail; }
        local.sources[i].family = (PortFamily)fam;
        local.sources[i].field_width = (size_t)field_width;
        local.sources[i].field_count = (size_t)field_count;
        local.sources[i].tag[0] = '\0';
        if (strcmp(tag_tok, "-") != 0) {
            if (port_set_tag(&local.sources[i], tag_tok) != 0) {
                goto fail;
            }
        }
    }

    /* LHS chain */
    if (fscanf(f, "%15s %lu", kw, &n_lhs) != 2) { goto fail; }
    if (strcmp(kw, "LHS") != 0) { goto fail; }
    if (n_lhs == 0 || n_lhs > PROPERTY_MAX_STEPS) { goto fail; }
    local.lhs_len = (size_t)n_lhs;
    for (i = 0; i < local.lhs_len; ++i) {
        if (fscanf(f, "%63s", local.lhs[i]) != 1) { goto fail; }
        if (!name_valid(local.lhs[i])) { goto fail; }
    }

    /* RHS chain */
    if (fscanf(f, "%15s %lu", kw, &n_rhs) != 2) { goto fail; }
    if (strcmp(kw, "RHS") != 0) { goto fail; }
    if (n_rhs > PROPERTY_MAX_STEPS) { goto fail; }
    local.rhs_len = (size_t)n_rhs;
    for (i = 0; i < local.rhs_len; ++i) {
        if (fscanf(f, "%63s", local.rhs[i]) != 1) { goto fail; }
        if (!name_valid(local.rhs[i])) { goto fail; }
    }

    fclose(f);
    *p = local;
    return 0;

fail:
    fclose(f);
    return -1;
}

/* ---- property_check ------------------------------------------------------- */

static const BinaryTransformNetwork *resolve(const PrimitiveRegistry *reg,
                                             const char *name) {
    size_t i;

    for (i = 0; i < reg->count; ++i) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            return reg->entries[i].btn;
        }
    }
    return NULL;
}

/* Position-wise compatibility for a handoff: same port count, each
   produced port port_compatible with the consuming port. */
static int seq_compatible(const Port *prod, size_t np,
                          const Port *cons, size_t nc) {
    size_t i;

    if (np != nc) {
        return 0;
    }
    for (i = 0; i < np; ++i) {
        if (!port_compatible(prod[i], cons[i])) {
            return 0;
        }
    }
    return 1;
}

/* The equation compares VALUES: representation must agree exactly, tags
   are free to differ. */
static int seq_same_representation(const Port *a, size_t na,
                                   const Port *b, size_t nb) {
    size_t i;

    if (na != nb) {
        return 0;
    }
    for (i = 0; i < na; ++i) {
        if (a[i].family != b[i].family ||
            a[i].field_width != b[i].field_width ||
            a[i].field_count != b[i].field_count) {
            return 0;
        }
    }
    return 1;
}

/* Strictly validate and canonicalize vec against a port sequence,
   writing the canonical form to clean. Returns 0, or -1 on any
   out-of-domain slice. */
static int seq_canonicalize(const Port *ports, size_t n,
                            const double *vec, double *clean) {
    size_t off = 0;
    size_t i;

    for (i = 0; i < n; ++i) {
        if (!port_validate(ports[i], vec + off) ||
            port_canonicalize(ports[i], vec + off, clean + off) != 0) {
            return -1;
        }
        off += plan_port_total(ports[i]);
    }
    return 0;
}

/* Run a chain on a canonical input vector, strict at every handoff.
   Returns 0 with the canonical result in out, -1 on any unclean value.
   n_steps 0 = identity (copies in to out). */
static int chain_run(const BinaryTransformNetwork *const *steps,
                     size_t n_steps, const double *in, size_t in_total,
                     double *out, double *buf_a, double *buf_b) {
    const double *cur = in;
    size_t s;

    if (n_steps == 0) {
        memcpy(out, in, in_total * sizeof *out);
        return 0;
    }
    for (s = 0; s < n_steps; ++s) {
        BinaryTransformNetwork *p = (BinaryTransformNetwork *)steps[s];
        double *stage = (s % 2 == 0) ? buf_a : buf_b;
        double *dest;
        const double *raw;

        if (seq_canonicalize(p->input_ports, p->input_port_count,
                             cur, stage) != 0) {
            return -1;
        }
        raw = btn_forward(p, stage);
        /* Writing the canonical output over the stage buffer is safe
           only if btn_forward has fully consumed its input by the time
           it returns: VERIFY against src/nn.c that btn_forward reads
           inputs only while computing hidden activations and that raw
           points at btn-internal memory (last_output), not at stage.
           If that does not hold, switch to a third buffer.
           VERIFICATION (src/nn.c lines 706-734): the hidden-activation
           loop (lines 711-720) reads inputs[input] fully before writing
           anything; the output loop (lines 722-731) reads only
           btn->hidden_output[], not inputs; line 733 returns
           btn->last_output (btn-internal). Both conditions hold:
           stage-overwrite is safe. */
        dest = (s + 1 == n_steps) ? out : stage;
        if (raw == NULL ||
            seq_canonicalize(p->output_ports, p->output_port_count,
                             raw, dest) != 0) {
            return -1;
        }
        cur = dest;
    }
    return 0;
}

int property_check(const Property *p, const PrimitiveRegistry *reg,
                   size_t max_samples, PropertyReport *report) {
    const BinaryTransformNetwork *lhs[PROPERTY_MAX_STEPS];
    const BinaryTransformNetwork *rhs[PROPERTY_MAX_STEPS];
    PlanDomain dom;
    double *in_vec = NULL;
    double *lhs_out = NULL;
    double *rhs_out = NULL;
    double *buf_a = NULL;
    double *buf_b = NULL;
    size_t max_width;
    size_t out_total;
    const Port *lhs_final;
    size_t lhs_final_n;
    const Port *rhs_final;
    size_t rhs_final_n;
    Arena arena;
    size_t s, i;
    int rc = -1;

    if (report != NULL) {
        memset(report, 0, sizeof *report);
    }
    if (p == NULL || reg == NULL ||
        p->source_count == 0 || p->source_count > PROPERTY_MAX_SOURCES ||
        p->lhs_len == 0 || p->lhs_len > PROPERTY_MAX_STEPS ||
        p->rhs_len > PROPERTY_MAX_STEPS) {
        return -1;
    }

    /* 1. RESOLVE */
    for (s = 0; s < p->lhs_len; ++s) {
        if ((lhs[s] = resolve(reg, p->lhs[s])) == NULL) {
            return -1;
        }
    }
    for (s = 0; s < p->rhs_len; ++s) {
        if ((rhs[s] = resolve(reg, p->rhs[s])) == NULL) {
            return -1;
        }
    }

    /* 2. STATIC TYPE GATE */
    {
        const Port *prev = p->sources;
        size_t prev_n = p->source_count;

        for (s = 0; s < p->lhs_len; ++s) {
            if (!seq_compatible(prev, prev_n, lhs[s]->input_ports,
                                lhs[s]->input_port_count)) {
                return -1;
            }
            prev = lhs[s]->output_ports;
            prev_n = lhs[s]->output_port_count;
        }
        lhs_final = prev;
        lhs_final_n = prev_n;

        prev = p->sources;
        prev_n = p->source_count;
        for (s = 0; s < p->rhs_len; ++s) {
            if (!seq_compatible(prev, prev_n, rhs[s]->input_ports,
                                rhs[s]->input_port_count)) {
                return -1;
            }
            prev = rhs[s]->output_ports;
            prev_n = rhs[s]->output_port_count;
        }
        rhs_final = prev;
        rhs_final_n = prev_n;

        if (!seq_same_representation(lhs_final, lhs_final_n,
                                     rhs_final, rhs_final_n)) {
            return -1;
        }
    }

    /* 3. REPLAY over the enumerated domain */
    if (plan_domain_build(p->sources, p->source_count, &dom) != 0) {
        return -1;
    }
    if (dom.combos > max_samples) {
        plan_domain_free(&dom);
        return -1;
    }

    out_total = 0;
    for (i = 0; i < lhs_final_n; ++i) {
        out_total += plan_port_total(lhs_final[i]);
    }
    max_width = dom.in_total > out_total ? dom.in_total : out_total;
    for (s = 0; s < p->lhs_len; ++s) {
        if (lhs[s]->input_count > max_width) max_width = lhs[s]->input_count;
        if (lhs[s]->output_count > max_width) max_width = lhs[s]->output_count;
    }
    for (s = 0; s < p->rhs_len; ++s) {
        if (rhs[s]->input_count > max_width) max_width = rhs[s]->input_count;
        if (rhs[s]->output_count > max_width) max_width = rhs[s]->output_count;
    }

    arena_init(&arena);
    in_vec = arena_alloc(&arena, dom.in_total * sizeof *in_vec);
    lhs_out = arena_alloc(&arena, out_total * sizeof *lhs_out);
    rhs_out = arena_alloc(&arena, out_total * sizeof *rhs_out);
    buf_a = arena_alloc(&arena, max_width * sizeof *buf_a);
    buf_b = arena_alloc(&arena, max_width * sizeof *buf_b);
    if (in_vec == NULL || lhs_out == NULL || rhs_out == NULL ||
        buf_a == NULL || buf_b == NULL) {
        goto done;
    }

    {
        size_t violated = 0;

        for (s = 0; s < dom.combos; ++s) {
            int clean;

            plan_domain_write(&dom, s, in_vec);
            clean = chain_run(lhs, p->lhs_len, in_vec, dom.in_total,
                              lhs_out, buf_a, buf_b) == 0 &&
                    chain_run(rhs, p->rhs_len, in_vec, dom.in_total,
                              rhs_out, buf_a, buf_b) == 0;
            if (clean) {
                for (i = 0; i < out_total; ++i) {
                    if (lhs_out[i] != rhs_out[i]) {
                        clean = 0;
                        break;
                    }
                }
            }
            if (report != NULL) {
                ++report->inputs;
                if (clean) {
                    ++report->held;
                } else {
                    ++report->violated;
                }
            }
            if (!clean) {
                ++violated;
            }
        }
        rc = violated == 0 ? 0 : -1;
    }

done:
    arena_reset(&arena);
    plan_domain_free(&dom);
    return rc;
}
