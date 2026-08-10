/* Long-run length + collapse probe for chess-fetch generate_fifo */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/cnet_generate_fifo.h"
#include "../include/contract/contract.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/specialist.h"

static const char *TEACHER = "hello_cnet_chess_fifo_neurons_generate_text_not_parrot";

static double wall_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static void oh(double *v, int hot, int n) {
    int i;
    for (i = 0; i < n; i++) v[i] = (i == hot) ? 1.0 : 0.0;
}

static int unit_exists(const PrimitiveRegistry *reg, const char *name) {
    size_t e;
    for (e = 0; e < reg->count; e++)
        if (reg->entries[e].name && strcmp(reg->entries[e].name, name) == 0) return 1;
    return 0;
}

static int build_alphabet(const char *s, char *charset, int *cmap, int maxA) {
    int n = 0, i;
    memset(cmap, -1, 256 * sizeof(int));
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (cmap[c] >= 0) continue;
        if (n >= maxA) break;
        cmap[c] = n;
        charset[n++] = (char)c;
    }
    charset[n] = '\0';
    return n;
}

static int admit_transition(PrimitiveRegistry *reg, HybridAi *h, Port pin, Port pout,
                            const char *name, int A, int from, int to) {
    BinaryTransformNetwork *btn;
    double in[CNET_GEN_MAX_UNITS], tg[CNET_GEN_MAX_UNITS];
    Contract c;
    Specialist s;
    char *stable;
    unsigned seed = 11u + (unsigned)from * 17u + (unsigned)to;

    btn = (BinaryTransformNetwork *)calloc(1, sizeof *btn);
    if (!btn) return -1;
    if (btn_init(btn, (size_t)A, (size_t)A, 12, 48, 0.5, seed) != 0) {
        free(btn);
        return -1;
    }
    if (btn_set_ports(btn, pin, pout) != 0) {
        btn_free(btn);
        free(btn);
        return -1;
    }
    oh(in, from, A);
    oh(tg, to, A);
    btn_train_dynamic(btn, in, tg, 1, 25000, 100, 1e-9, 1e-12);
    btn_train(btn, in, tg, 1, 8000);
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, name, btn, in, tg, 1) != 0) {
        btn_free(btn);
        free(btn);
        return -1;
    }
    stable = (char *)malloc(strlen(name) + 1);
    if (!stable) {
        contract_free(&c);
        btn_free(btn);
        free(btn);
        return -1;
    }
    memcpy(stable, name, strlen(name) + 1);
    memset(&s, 0, sizeof s);
    if (specialist_wrap_btn(&s, btn, stable) != 0 || specialist_admit(reg, &s, &c) != 0) {
        free(stable);
        contract_free(&c);
        btn_free(btn);
        free(btn);
        return -1;
    }
    if (hybrid_coverage_record(h, pin, pout, name, in, tg, 1, (size_t)A, (size_t)A) != 0) {
        contract_free(&c);
        return -1;
    }
    contract_free(&c);
    return 0;
}

static void bind_all(CnetGenEngine *e, const PrimitiveRegistry *reg, const char *charset, int A) {
    int i;
    for (i = 0; i < A; i++) {
        char name[64];
        snprintf(name, sizeof name, "neuron_%c", charset[i]);
        if (unit_exists(reg, name)) cnet_gen_bind(e, i, name);
    }
}

/* Collapse: first index i where out[i..i+run) is same char for run>=R, or -1 */
static int collapse_point(const char *s, size_t n, int run) {
    size_t i, j;
    if (n < (size_t)run) return -1;
    for (i = 0; i + (size_t)run <= n; i++) {
        int ok = 1;
        for (j = 1; j < (size_t)run; j++)
            if (s[i + j] != s[i]) {
                ok = 0;
                break;
            }
        if (ok) return (int)i;
    }
    return -1;
}

static int unique_chars(const char *s, size_t n) {
    int seen[256], u = 0;
    size_t i;
    memset(seen, 0, sizeof seen);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!seen[c]) {
            seen[c] = 1;
            u++;
        }
    }
    return u;
}

static void run_one(const char *label, CnetGenEngine *e, HybridAi *cov, PrimitiveRegistry *reg,
                    int start, int steps, const char *charset, int mode) {
    char *out;
    size_t n = 0, cap;
    double t0, t1;
    int cp8, cp32, cp128;
    size_t fw0 = e->n_forwards, em0 = e->n_emit, ab0 = e->n_abstain, fe0 = e->n_fetch;

    cap = (size_t)steps + 64;
    out = (char *)malloc(cap);
    if (!out) {
        printf("OOM\n");
        return;
    }
    /* clear fifo */
    while (e->fifo.count) {
        char c;
        cnet_gen_fifo_pop(&e->fifo, &c);
    }
    t0 = wall_s();
    cnet_gen_run(e, cov, reg, start, steps, mode, charset, out, cap, &n);
    t1 = wall_s();

    cp8 = collapse_point(out, n, 8);
    cp32 = collapse_point(out, n, 32);
    cp128 = collapse_point(out, n, 128);

    printf("\n== %s steps=%d ==\n", label, steps);
    printf("  len=%zu requested=%d full=%s\n", n, steps, n == (size_t)steps ? "yes" : "no");
    printf("  wall=%.4fs forwards=%zu emit=%zu fetch=%zu abstain=%zu\n", t1 - t0,
           e->n_forwards - fw0, e->n_emit - em0, e->n_fetch - fe0, e->n_abstain - ab0);
    printf("  unique_chars=%d\n", unique_chars(out, n));
    printf("  collapse_run8_at=%d  run32_at=%d  run128_at=%d\n", cp8, cp32, cp128);
    if (n > 0) {
        size_t head = n < 64 ? n : 64;
        size_t tail = n < 64 ? n : 64;
        char hbuf[80], tbuf[80];
        memcpy(hbuf, out, head);
        hbuf[head] = 0;
        memcpy(tbuf, out + n - tail, tail);
        tbuf[tail] = 0;
        printf("  head=\"%s\"\n", hbuf);
        printf("  tail=\"%s\"\n", tbuf);
        if (cp8 >= 0 && (size_t)cp8 < n)
            printf("  collapse_char='%c'\n", out[cp8]);
    }
    free(out);
}

int main(void) {
    char charset[CNET_GEN_MAX_UNITS + 1];
    int cmap[256], ids[512];
    int A, i, L, placed = 0, start;
    HybridAi cov;
    PrimitiveRegistry reg;
    CnetGenEngine eng;
    Port pin, pout;
    int lengths[] = {48, 512, 4096};
    int nlen = 3;

    printf("GENERATE_FIFO_LONG_EXP\n");
    printf("teacher=\"%s\"\n", TEACHER);

    A = build_alphabet(TEACHER, charset, cmap, CNET_GEN_MAX_UNITS);
    L = (int)strlen(TEACHER);
    for (i = 0; i < L; i++) ids[i] = cmap[(unsigned char)TEACHER[i]];
    start = ids[0];

    hybrid_ai_init(&cov);
    registry_init(&reg);
    memset(&pin, 0, sizeof pin);
    memset(&pout, 0, sizeof pout);
    pin.family = pout.family = PORT_ONEHOT;
    pin.field_width = pout.field_width = (size_t)A;
    pin.field_count = pout.field_count = 1;
    snprintf(pin.tag, sizeof pin.tag, "gen_in");
    snprintf(pout.tag, sizeof pout.tag, "gen_out");

    for (i = 0; i < L - 1; i++) {
        char name[64];
        int from = ids[i], to = ids[i + 1];
        snprintf(name, sizeof name, "neuron_%c", charset[from]);
        if (unit_exists(&reg, name)) continue;
        if (admit_transition(&reg, &cov, pin, pout, name, A, from, to) != 0) {
            fprintf(stderr, "admit fail\n");
            return 1;
        }
        placed++;
    }
    printf("placed_neurons=%d alphabet=%d teacher_len=%d charset=\"%s\"\n", placed, A, L,
           charset);

    /* large FIFO for 4096 */
    cnet_gen_engine_init(&eng, A, 8192);
    eng.in_port = pin;
    eng.out_port = pout;
    bind_all(&eng, &reg, charset, A);

    for (i = 0; i < nlen; i++) {
        /* fresh engine stats per run — re-init bind */
        CnetGenEngine e2;
        cnet_gen_engine_init(&e2, A, 8192);
        e2.in_port = pin;
        e2.out_port = pout;
        bind_all(&e2, &reg, charset, A);
        run_one("chess", &e2, &cov, &reg, start, lengths[i], charset, 0);
        cnet_gen_engine_free(&e2);
    }

    /* one resort at 512 only (4096 would be slow-ish but ok) */
    {
        CnetGenEngine e2;
        cnet_gen_engine_init(&e2, A, 8192);
        e2.in_port = pin;
        e2.out_port = pout;
        bind_all(&e2, &reg, charset, A);
        run_one("resort", &e2, &cov, &reg, start, 512, charset, 1);
        cnet_gen_engine_free(&e2);
    }

    cnet_gen_engine_free(&eng);
    hybrid_ai_free(&cov);
    registry_free(&reg);
    printf("\nGENERATE_FIFO_LONG_EXP_DONE\n");
    return 0;
}
