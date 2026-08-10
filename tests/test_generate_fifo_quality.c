/* Quality lever: position-board pieces vs char-Markov.
 * make generate_fifo_quality -> GENERATE_FIFO_QUALITY_PASS
 */
#include <math.h>
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

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-62s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

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

/* Map onehot(in_hot) of in_dim -> onehot(out_hot) of out_dim */
static int admit_map(PrimitiveRegistry *reg, HybridAi *h, Port pin, Port pout,
                     const char *name, int in_dim, int out_dim, int in_hot, int out_hot) {
    BinaryTransformNetwork *btn;
    double *in, *tg;
    Contract c;
    Specialist s;
    char *stable;
    unsigned seed = 13u + (unsigned)in_hot * 31u + (unsigned)out_hot * 7u;

    in = (double *)calloc((size_t)in_dim, sizeof(double));
    tg = (double *)calloc((size_t)out_dim, sizeof(double));
    btn = (BinaryTransformNetwork *)calloc(1, sizeof *btn);
    if (!in || !tg || !btn) {
        free(in);
        free(tg);
        free(btn);
        return -1;
    }
    if (btn_init(btn, (size_t)in_dim, (size_t)out_dim, 16, 64, 0.5, seed) != 0) {
        free(in);
        free(tg);
        free(btn);
        return -1;
    }
    if (btn_set_ports(btn, pin, pout) != 0) {
        btn_free(btn);
        free(btn);
        free(in);
        free(tg);
        return -1;
    }
    oh(in, in_hot, in_dim);
    oh(tg, out_hot, out_dim);
    btn_train_dynamic(btn, in, tg, 1, 30000, 100, 1e-9, 1e-12);
    btn_train(btn, in, tg, 1, 10000);
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, name, btn, in, tg, 1) != 0) {
        btn_free(btn);
        free(btn);
        free(in);
        free(tg);
        return -1;
    }
    stable = (char *)malloc(strlen(name) + 1);
    if (!stable) {
        contract_free(&c);
        btn_free(btn);
        free(btn);
        free(in);
        free(tg);
        return -1;
    }
    memcpy(stable, name, strlen(name) + 1);
    memset(&s, 0, sizeof s);
    if (specialist_wrap_btn(&s, btn, stable) != 0 || specialist_admit(reg, &s, &c) != 0) {
        free(stable);
        contract_free(&c);
        btn_free(btn);
        free(btn);
        free(in);
        free(tg);
        return -1;
    }
    if (hybrid_coverage_record(h, pin, pout, name, in, tg, 1, (size_t)in_dim,
                               (size_t)out_dim) != 0) {
        contract_free(&c);
        free(in);
        free(tg);
        return -1;
    }
    contract_free(&c);
    free(in);
    free(tg);
    return 0;
}

static int collapse_at(const char *s, size_t n, int run) {
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

/* Match rate vs teacher suffix (emitted text should equal teacher[1..]) for position */
static double match_teacher(const char *out, size_t n, const char *teacher) {
    size_t i, ok = 0, m;
    if (!out || !teacher || n == 0) return 0.0;
    m = strlen(teacher);
    if (m < 2) return 0.0;
    /* position mode emits teacher[1], teacher[2], ... */
    for (i = 0; i < n && i + 1 < m; i++)
        if (out[i] == teacher[i + 1]) ok++;
    return (double)ok / (double)n;
}

int main(void) {
    char charset[CNET_GEN_MAX_UNITS + 1];
    int cmap[256], ids[512];
    int A, L, i, n_pos;
    HybridAi cov_m, cov_p;
    PrimitiveRegistry reg_m, reg_p;
    Port pin_m, pout_m, pin_p, pout_p;
    char out_m[512], out_p[512], out_p_long[8192];
    size_t nm = 0, np = 0, npl = 0;
    double t0, t1, match_m, match_p, match_pl;
    int cp_m, cp_p, cp_pl;

    failures = checks = 0;
    printf("== generate_fifo QUALITY: markov vs position board ==\n");
    printf("teacher=\"%s\"\n", TEACHER);

    A = build_alphabet(TEACHER, charset, cmap, CNET_GEN_MAX_UNITS);
    L = (int)strlen(TEACHER);
    for (i = 0; i < L; i++) ids[i] = cmap[(unsigned char)TEACHER[i]];
    n_pos = L - 1; /* squares 0..L-2 emit teacher[1..L-1] */
    check(A >= 4 && n_pos >= 4 && n_pos <= CNET_GEN_MAX_UNITS, "dims ok");

    /* --- Markov place (char -> next char) --- */
    hybrid_ai_init(&cov_m);
    registry_init(&reg_m);
    memset(&pin_m, 0, sizeof pin_m);
    memset(&pout_m, 0, sizeof pout_m);
    pin_m.family = pout_m.family = PORT_ONEHOT;
    pin_m.field_width = pout_m.field_width = (size_t)A;
    pin_m.field_count = pout_m.field_count = 1;
    snprintf(pin_m.tag, sizeof pin_m.tag, "gen_in");
    snprintf(pout_m.tag, sizeof pout_m.tag, "gen_out");
    for (i = 0; i < L - 1; i++) {
        char name[64];
        snprintf(name, sizeof name, "m_%c", charset[ids[i]]);
        if (unit_exists(&reg_m, name)) continue;
        check(admit_map(&reg_m, &cov_m, pin_m, pout_m, name, A, A, ids[i], ids[i + 1]) == 0,
              i == 0 ? "admit markov neuron" : "admit m");
    }

    /* --- Position place (square i -> char teacher[i+1]) --- */
    hybrid_ai_init(&cov_p);
    registry_init(&reg_p);
    memset(&pin_p, 0, sizeof pin_p);
    memset(&pout_p, 0, sizeof pout_p);
    pin_p.family = pout_p.family = PORT_ONEHOT;
    pin_p.field_width = (size_t)n_pos;
    pout_p.field_width = (size_t)A;
    pin_p.field_count = pout_p.field_count = 1;
    snprintf(pin_p.tag, sizeof pin_p.tag, "gen_in");
    snprintf(pout_p.tag, sizeof pout_p.tag, "gen_out");
    for (i = 0; i < n_pos; i++) {
        char name[64];
        snprintf(name, sizeof name, "pos_%02d", i);
        check(admit_map(&reg_p, &cov_p, pin_p, pout_p, name, n_pos, A, i, ids[i + 1]) == 0,
              i == 0 ? "admit position piece" : "admit p");
    }
    printf("  markov_units=%zu position_squares=%d alphabet=%d teacher_len=%d\n", reg_m.count,
           n_pos, A, L);

    /* Run markov chess */
    {
        CnetGenEngine e;
        cnet_gen_engine_init(&e, A, 2048);
        e.in_port = pin_m;
        e.out_port = pout_m;
        cnet_gen_engine_set_advance(&e, CNET_GEN_ADVANCE_MARKOV, 0);
        for (i = 0; i < A; i++) {
            char name[64];
            snprintf(name, sizeof name, "m_%c", charset[i]);
            if (unit_exists(&reg_m, name)) cnet_gen_bind(&e, i, name);
        }
        t0 = wall_s();
        cnet_gen_run(&e, &cov_m, &reg_m, ids[0], n_pos, 0, charset, out_m, sizeof out_m, &nm);
        t1 = wall_s();
        match_m = match_teacher(out_m, nm, TEACHER);
        cp_m = collapse_at(out_m, nm, 8);
        printf("\n-- MARKOV (char state) --\n");
        printf("  len=%zu wall=%.4fs match_teacher=%.3f unique=%d collapse8=%d\n", nm, t1 - t0,
               match_m, unique_chars(out_m, nm), cp_m);
        printf("  out=\"%s\"\n", out_m);
        cnet_gen_engine_free(&e);
    }

    /* Run position chess — exact teacher path */
    {
        CnetGenEngine e;
        cnet_gen_engine_init(&e, A, 2048);
        cnet_gen_engine_set_dims(&e, n_pos, A);
        e.in_port = pin_p;
        e.out_port = pout_p;
        cnet_gen_engine_set_advance(&e, CNET_GEN_ADVANCE_POSITION, n_pos);
        for (i = 0; i < n_pos; i++) {
            char name[64];
            snprintf(name, sizeof name, "pos_%02d", i);
            cnet_gen_bind(&e, i, name);
        }
        t0 = wall_s();
        cnet_gen_run(&e, &cov_p, &reg_p, 0, n_pos, 0, charset, out_p, sizeof out_p, &np);
        t1 = wall_s();
        match_p = match_teacher(out_p, np, TEACHER);
        cp_p = collapse_at(out_p, np, 8);
        printf("\n-- POSITION (board squares) --\n");
        printf("  len=%zu wall=%.4fs match_teacher=%.3f unique=%d collapse8=%d\n", np, t1 - t0,
               match_p, unique_chars(out_p, np), cp_p);
        printf("  out=\"%s\"\n", out_p);
        printf("  expect=\"%s\"\n", TEACHER + 1);
        cnet_gen_engine_free(&e);
    }

    /* Position long request: should emit n_pos then abstain (not infinite llll) */
    {
        CnetGenEngine e;
        cnet_gen_engine_init(&e, A, 8192);
        cnet_gen_engine_set_dims(&e, n_pos, A);
        e.in_port = pin_p;
        e.out_port = pout_p;
        cnet_gen_engine_set_advance(&e, CNET_GEN_ADVANCE_POSITION, n_pos);
        for (i = 0; i < n_pos; i++) {
            char name[64];
            snprintf(name, sizeof name, "pos_%02d", i);
            cnet_gen_bind(&e, i, name);
        }
        cnet_gen_run(&e, &cov_p, &reg_p, 0, 4096, 0, charset, out_p_long, sizeof out_p_long,
                     &npl);
        match_pl = match_teacher(out_p_long, npl, TEACHER);
        cp_pl = collapse_at(out_p_long, npl, 8);
        printf("\n-- POSITION long ask=4096 --\n");
        printf("  len=%zu (cap at board) abstain=%zu match=%.3f collapse8=%d unique=%d\n", npl,
               e.n_abstain, match_pl, cp_pl, unique_chars(out_p_long, npl));
        check(npl == (size_t)n_pos, "long run stops at board end (not 4096 garbage)");
        check(e.n_abstain >= 1, "abstain after last square");
        cnet_gen_engine_free(&e);
    }

    check(nm > 0 && np > 0, "both modes emit");
    check(match_p >= 0.95, "position match_teacher >= 0.95");
    check(match_p > match_m + 0.2 || match_m < 0.5, "position quality >> markov collapse");
    check(strcmp(out_p, TEACHER + 1) == 0 || match_p >= 0.99, "position nearly exact teacher path");
    check(cp_m >= 0, "markov still collapses (control)");
    /* position teacher text may have repeated letters but not total collapse from step 1 */
    check(unique_chars(out_p, np) >= 10, "position keeps teacher diversity");

    hybrid_ai_free(&cov_m);
    hybrid_ai_free(&cov_p);
    registry_free(&reg_m);
    registry_free(&reg_p);

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("GENERATE_FIFO_QUALITY_PASS\n");
        return 0;
    }
    printf("GENERATE_FIFO_QUALITY_FAIL\n");
    return 1;
}
