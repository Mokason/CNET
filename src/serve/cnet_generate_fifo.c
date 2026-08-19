#include "../../include/cnet_generate_fifo.h"
#include "../../include/cnet_sparse_serve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cnet_gen_fifo_init(CnetGenFifo *f, size_t cap) {
    memset(f, 0, sizeof *f);
    if (cap < 8) cap = 8;
    f->buf = (char *)malloc(cap);
    f->cap = f->buf ? cap : 0;
}

void cnet_gen_fifo_free(CnetGenFifo *f) {
    if (!f) return;
    free(f->buf);
    memset(f, 0, sizeof *f);
}

int cnet_gen_fifo_push(CnetGenFifo *f, char c) {
    if (!f || !f->buf || f->cap == 0) return -1;
    if (f->count >= f->cap) {
        f->dropped++;
        return -1;
    }
    f->buf[f->tail] = c;
    f->tail = (f->tail + 1) % f->cap;
    f->count++;
    return 0;
}

int cnet_gen_fifo_pop(CnetGenFifo *f, char *c_out) {
    if (!f || !f->buf || f->count == 0) return -1;
    if (c_out) *c_out = f->buf[f->head];
    f->head = (f->head + 1) % f->cap;
    f->count--;
    return 0;
}

size_t cnet_gen_fifo_drain(CnetGenFifo *f, char *dst, size_t dst_cap) {
    size_t n = 0;
    if (!f || !dst || dst_cap == 0) return 0;
    while (n + 1 < dst_cap && f->count > 0) {
        char c;
        if (cnet_gen_fifo_pop(f, &c) != 0) break;
        dst[n++] = c;
    }
    dst[n] = '\0';
    return n;
}

static void set_ports(CnetGenEngine *e) {
    memset(&e->in_port, 0, sizeof e->in_port);
    memset(&e->out_port, 0, sizeof e->out_port);
    e->in_port.family = PORT_ONEHOT;
    e->out_port.family = PORT_ONEHOT;
    e->in_port.field_width = (size_t)(e->state_dim > 0 ? e->state_dim : 1);
    e->out_port.field_width = (size_t)(e->alphabet > 0 ? e->alphabet : 1);
    e->in_port.field_count = e->out_port.field_count = 1;
    snprintf(e->in_port.tag, sizeof e->in_port.tag, "gen_in");
    snprintf(e->out_port.tag, sizeof e->out_port.tag, "gen_out");
}

void cnet_gen_engine_init(CnetGenEngine *e, int alphabet, size_t fifo_cap) {
    memset(e, 0, sizeof *e);
    e->alphabet = alphabet > 0 ? alphabet : 1;
    if (e->alphabet > CNET_GEN_MAX_UNITS) e->alphabet = CNET_GEN_MAX_UNITS;
    e->state_dim = e->alphabet;
    e->advance = CNET_GEN_ADVANCE_MARKOV;
    e->pos_limit = 0;
    cnet_gen_fifo_init(&e->fifo, fifo_cap ? fifo_cap : CNET_GEN_FIFO_DEFAULT);
    set_ports(e);
}

void cnet_gen_engine_set_dims(CnetGenEngine *e, int state_dim, int alphabet) {
    if (!e) return;
    if (state_dim > 0 && state_dim <= CNET_GEN_MAX_UNITS) e->state_dim = state_dim;
    if (alphabet > 0 && alphabet <= CNET_GEN_MAX_UNITS) e->alphabet = alphabet;
    set_ports(e);
}

void cnet_gen_engine_set_advance(CnetGenEngine *e, int advance, int pos_limit) {
    if (!e) return;
    e->advance = (advance == CNET_GEN_ADVANCE_POSITION) ? CNET_GEN_ADVANCE_POSITION
                                                         : CNET_GEN_ADVANCE_MARKOV;
    e->pos_limit = pos_limit > 0 ? pos_limit : 0;
}

void cnet_gen_engine_free(CnetGenEngine *e) {
    if (!e) return;
    cnet_gen_fifo_free(&e->fifo);
    memset(e, 0, sizeof *e);
}

int cnet_gen_bind(CnetGenEngine *e, int state_id, const char *unit_name) {
    int i;
    if (!e || !unit_name || !unit_name[0]) return -1;
    if (state_id < 0 || state_id >= e->state_dim) return -1;
    for (i = 0; i < e->n_bind; i++) {
        if (e->bind[i].active && e->bind[i].state_id == state_id) {
            snprintf(e->bind[i].name, sizeof e->bind[i].name, "%s", unit_name);
            return 0;
        }
    }
    if (e->n_bind >= CNET_GEN_MAX_UNITS) return -1;
    e->bind[e->n_bind].state_id = state_id;
    e->bind[e->n_bind].active = 1;
    snprintf(e->bind[e->n_bind].name, sizeof e->bind[e->n_bind].name, "%s", unit_name);
    e->n_bind++;
    return 0;
}

static const RegistryEntry *find_cert(const PrimitiveRegistry *reg, const char *name) {
    size_t i;
    if (!reg || !name) return NULL;
    for (i = 0; i < reg->count; i++) {
        if (reg->entries[i].name && strcmp(reg->entries[i].name, name) == 0 &&
            reg->entries[i].certified && reg->entries[i].btn)
            return &reg->entries[i];
    }
    return NULL;
}

static void oh(double *v, int hot, int n) {
    int i;
    for (i = 0; i < n; i++) v[i] = (i == hot) ? 1.0 : 0.0;
}

static int argmax(const double *v, int n) {
    int i, b = 0;
    for (i = 1; i < n; i++)
        if (v[i] > v[b]) b = i;
    return b;
}

static const CnetGenBinding *bind_for_state(const CnetGenEngine *e, int state) {
    int i;
    for (i = 0; i < e->n_bind; i++)
        if (e->bind[i].active && e->bind[i].state_id == state) return &e->bind[i];
    return NULL;
}

static int run_unit_emit(CnetGenEngine *e, const HybridAi *cov, const PrimitiveRegistry *reg,
                         const char *unit, int *state_io, const char *charset,
                         int count_forward) {
    const RegistryEntry *ent;
    const double *y;
    double x[CNET_GEN_MAX_UNITS];
    int next_char, Sd, A;
    char ch;

    if (!e || !reg || !state_io || !charset || !unit) return -1;
    Sd = e->state_dim;
    A = e->alphabet;
    if (*state_io < 0 || *state_io >= Sd) return -1;
    if ((int)strlen(charset) < A) return -1;

    if (e->advance == CNET_GEN_ADVANCE_POSITION && e->pos_limit > 0 &&
        *state_io >= e->pos_limit) {
        e->n_abstain++;
        e->n_steps++;
        return 1;
    }

    ent = find_cert(reg, unit);
    if (!ent || !ent->btn) {
        e->n_abstain++;
        return 1;
    }
    if ((int)ent->btn->input_count != Sd || (int)ent->btn->output_count != A) {
        e->n_abstain++;
        return 1;
    }

    oh(x, *state_io, Sd);
    {
        Port ip = e->in_port, op = e->out_port;
        if (ent->btn->input_port_count >= 1) ip = ent->btn->input_ports[0];
        if (ent->btn->output_port_count >= 1) op = ent->btn->output_ports[0];
        if (cov && hybrid_coverage_has_unit(cov, unit)) {
            if (!hybrid_coverage_admits_exact(cov, unit, ip, op, x, (size_t)Sd)) {
                e->n_abstain++;
                return 1;
            }
        }
    }

    y = btn_forward(ent->btn, x);
    if (count_forward) e->n_forwards++;
    if (!y) {
        e->n_abstain++;
        return 1;
    }
    next_char = argmax(y, A);
    ch = charset[next_char];
    if (cnet_gen_fifo_push(&e->fifo, ch) != 0) {
        e->n_abstain++;
        return 1;
    }
    e->n_emit++;
    e->n_steps++;
    if (e->advance == CNET_GEN_ADVANCE_POSITION)
        (*state_io)++;
    else
        *state_io = next_char; /* markov: char becomes next state */
    return 0;
}

int cnet_gen_step_chess(CnetGenEngine *e, const HybridAi *cov, const PrimitiveRegistry *reg,
                        int *state_io, const char *charset) {
    const CnetGenBinding *b;
    if (!e || !state_io) return -1;
    if (e->advance == CNET_GEN_ADVANCE_POSITION && e->pos_limit > 0 &&
        *state_io >= e->pos_limit) {
        e->n_abstain++;
        e->n_steps++;
        return 1;
    }
    b = bind_for_state(e, *state_io);
    if (!b) {
        e->n_abstain++;
        e->n_steps++;
        return 1;
    }
    e->n_fetch++;
    return run_unit_emit(e, cov, reg, b->name, state_io, charset, 1);
}

int cnet_gen_step_resort(CnetGenEngine *e, const HybridAi *cov, const PrimitiveRegistry *reg,
                         int *state_io, const char *charset) {
    int i, Sd, A, best_i = -1;
    double best_score = -1e300;
    double x[CNET_GEN_MAX_UNITS];
    char best_name[CNET_GEN_NAME_MAX];

    if (!e || !state_io || !reg) return -1;
    Sd = e->state_dim;
    A = e->alphabet;
    if (*state_io < 0 || *state_io >= Sd) return -1;
    if (e->advance == CNET_GEN_ADVANCE_POSITION && e->pos_limit > 0 &&
        *state_io >= e->pos_limit) {
        e->n_abstain++;
        e->n_steps++;
        return 1;
    }
    oh(x, *state_io, Sd);
    best_name[0] = '\0';

    for (i = 0; i < e->n_bind; i++) {
        const RegistryEntry *ent;
        const double *y;
        double peak;
        int j;
        if (!e->bind[i].active) continue;
        ent = find_cert(reg, e->bind[i].name);
        if (!ent || !ent->btn) continue;
        if ((int)ent->btn->input_count != Sd || (int)ent->btn->output_count != A) continue;
        if (cov && hybrid_coverage_has_unit(cov, e->bind[i].name)) {
            if (!hybrid_coverage_admits_exact(cov, e->bind[i].name, e->in_port, e->out_port,
                                             x, (size_t)Sd))
                continue;
        }
        y = btn_forward(ent->btn, x);
        e->n_forwards++;
        e->n_resort++;
        if (!y) continue;
        peak = y[0];
        for (j = 1; j < A; j++)
            if (y[j] > peak) peak = y[j];
        if (e->bind[i].state_id == *state_io) peak += 1e-6;
        if (peak > best_score) {
            best_score = peak;
            best_i = i;
            snprintf(best_name, sizeof best_name, "%s", e->bind[i].name);
        }
    }
    if (best_i < 0 || !best_name[0]) {
        e->n_abstain++;
        e->n_steps++;
        return 1;
    }
    return run_unit_emit(e, cov, reg, best_name, state_io, charset, 1);
}

int cnet_gen_run(CnetGenEngine *e, const HybridAi *cov, const PrimitiveRegistry *reg,
                 int start_state, int n_steps, int mode_resort, const char *charset,
                 char *out, size_t out_cap, size_t *out_len) {
    int st = start_state;
    int t;
    if (!e || !reg || n_steps < 0) return -1;
    for (t = 0; t < n_steps; t++) {
        int rc = mode_resort ? cnet_gen_step_resort(e, cov, reg, &st, charset)
                             : cnet_gen_step_chess(e, cov, reg, &st, charset);
        if (rc < 0) return -1;
        if (rc == 1) break;
    }
    if (out && out_cap)
        *out_len = cnet_gen_fifo_drain(&e->fifo, out, out_cap);
    else if (out_len)
        *out_len = e->fifo.count;
    return 0;
}
