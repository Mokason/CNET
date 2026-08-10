/* Wall-time: teacher phrase -> CERT position pieces ready to generate */
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
    charset[n] = 0;
    return n;
}

static int admit_map(PrimitiveRegistry *reg, HybridAi *h, Port pin, Port pout,
                     const char *name, int in_dim, int out_dim, int in_hot, int out_hot) {
    BinaryTransformNetwork *btn;
    double *in, *tg;
    Contract c;
    Specialist s;
    char *stable;
    unsigned seed = 13u + (unsigned)in_hot * 31u + (unsigned)out_hot * 7u;
    in = calloc((size_t)in_dim, sizeof(double));
    tg = calloc((size_t)out_dim, sizeof(double));
    btn = calloc(1, sizeof *btn);
    if (!in || !tg || !btn) { free(in); free(tg); free(btn); return -1; }
    if (btn_init(btn, (size_t)in_dim, (size_t)out_dim, 16, 64, 0.5, seed) != 0) {
        free(in); free(tg); free(btn); return -1;
    }
    btn_set_ports(btn, pin, pout);
    oh(in, in_hot, in_dim);
    oh(tg, out_hot, out_dim);
    btn_train_dynamic(btn, in, tg, 1, 30000, 100, 1e-9, 1e-12);
    btn_train(btn, in, tg, 1, 10000);
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, name, btn, in, tg, 1) != 0) {
        btn_free(btn); free(btn); free(in); free(tg); return -1;
    }
    stable = malloc(strlen(name) + 1);
    memcpy(stable, name, strlen(name) + 1);
    memset(&s, 0, sizeof s);
    if (specialist_wrap_btn(&s, btn, stable) != 0 || specialist_admit(reg, &s, &c) != 0) {
        free(stable); contract_free(&c); btn_free(btn); free(btn); free(in); free(tg);
        return -1;
    }
    hybrid_coverage_record(h, pin, pout, name, in, tg, 1, (size_t)in_dim, (size_t)out_dim);
    contract_free(&c);
    free(in); free(tg);
    return 0;
}

int main(void) {
    char charset[256];
    int cmap[256], ids[512];
    int A, L, n_pos, i;
    HybridAi cov;
    PrimitiveRegistry reg;
    Port pin, pout;
    double t0, t1, t_place, t_gen;
    char out[512];
    size_t n = 0;
    CnetGenEngine e;

    A = build_alphabet(TEACHER, charset, cmap, 64);
    L = (int)strlen(TEACHER);
    for (i = 0; i < L; i++) ids[i] = cmap[(unsigned char)TEACHER[i]];
    n_pos = L - 1;

    hybrid_ai_init(&cov);
    registry_init(&reg);
    memset(&pin, 0, sizeof pin);
    memset(&pout, 0, sizeof pout);
    pin.family = pout.family = PORT_ONEHOT;
    pin.field_width = (size_t)n_pos;
    pout.field_width = (size_t)A;
    pin.field_count = pout.field_count = 1;
    snprintf(pin.tag, sizeof pin.tag, "gen_in");
    snprintf(pout.tag, sizeof pout.tag, "gen_out");

    t0 = wall_s();
    for (i = 0; i < n_pos; i++) {
        char name[64];
        snprintf(name, sizeof name, "pos_%02d", i);
        if (admit_map(&reg, &cov, pin, pout, name, n_pos, A, i, ids[i + 1]) != 0) {
            fprintf(stderr, "admit fail %d\n", i);
            return 1;
        }
    }
    t1 = wall_s();
    t_place = t1 - t0;

    cnet_gen_engine_init(&e, A, 2048);
    cnet_gen_engine_set_dims(&e, n_pos, A);
    e.in_port = pin;
    e.out_port = pout;
    cnet_gen_engine_set_advance(&e, CNET_GEN_ADVANCE_POSITION, n_pos);
    for (i = 0; i < n_pos; i++) {
        char name[64];
        snprintf(name, sizeof name, "pos_%02d", i);
        cnet_gen_bind(&e, i, name);
    }

    t0 = wall_s();
    cnet_gen_run(&e, &cov, &reg, 0, n_pos, 0, charset, out, sizeof out, &n);
    t1 = wall_s();
    t_gen = t1 - t0;

    printf("TEACHER_LEARN_TIMING\n");
    printf("teacher_len=%d n_pieces=%d alphabet=%d\n", L, n_pos, A);
    printf("place_train_admit_coverage_wall_s=%.4f\n", t_place);
    printf("place_per_piece_ms=%.2f\n", 1000.0 * t_place / (double)n_pos);
    printf("generate_after_learn_wall_s=%.4f len=%zu\n", t_gen, n);
    printf("match=%s\n", (n == (size_t)n_pos && strcmp(out, TEACHER + 1) == 0) ? "exact" : "partial");
    printf("total_learn_plus_one_gen_s=%.4f\n", t_place + t_gen);
    printf("out=\"%s\"\n", out);

    /* also: structure mine path times from learning_delta if available */
    cnet_gen_engine_free(&e);
    hybrid_ai_free(&cov);
    registry_free(&reg);
    return 0;
}
