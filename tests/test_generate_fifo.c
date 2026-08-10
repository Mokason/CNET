/* Experiment: chess-fetch + FIFO emit vs full re-sort each step.
 * Self-generated text from CERT neurons — not next-token parrot CE.
 *
 * make generate_fifo -> GENERATE_FIFO_PASS
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

static void bind_all(CnetGenEngine *e, const PrimitiveRegistry *reg, const char *charset,
                     int A) {
    int i;
    for (i = 0; i < A; i++) {
        char name[64];
        snprintf(name, sizeof name, "neuron_%c", charset[i]);
        if (unit_exists(reg, name)) cnet_gen_bind(e, i, name);
    }
}

static void fifo_clear(CnetGenFifo *f) {
    while (f->count) {
        char c;
        cnet_gen_fifo_pop(f, &c);
    }
}

int main(void) {
    char charset[CNET_GEN_MAX_UNITS + 1];
    int cmap[256];
    int A, i, L;
    int ids[256];
    HybridAi cov;
    PrimitiveRegistry reg;
    CnetGenEngine chess, resort;
    char out_c[512], out_r[512];
    size_t len_c = 0, len_r = 0;
    double t0, t1, chess_s, resort_s;
    int start, steps, rep, placed = 0;
    size_t fw_c, fw_r;
    const int REPS = 200;
    const int STEPS = 48;
    Port pin, pout;

    failures = checks = 0;
    printf("== generate_fifo experiment (chess-fetch vs re-sort) ==\n");
    printf("teacher=\"%s\"\n", TEACHER);

    A = build_alphabet(TEACHER, charset, cmap, CNET_GEN_MAX_UNITS);
    check(A >= 4, "alphabet built");
    L = (int)strlen(TEACHER);
    for (i = 0; i < L; i++) ids[i] = cmap[(unsigned char)TEACHER[i]];
    check(ids[0] >= 0, "teacher mapped");

    hybrid_ai_init(&cov);
    registry_init(&reg);

    memset(&pin, 0, sizeof pin);
    memset(&pout, 0, sizeof pout);
    pin.family = pout.family = PORT_ONEHOT;
    pin.field_width = pout.field_width = (size_t)A;
    pin.field_count = pout.field_count = 1;
    snprintf(pin.tag, sizeof pin.tag, "gen_in");
    snprintf(pout.tag, sizeof pout.tag, "gen_out");

    /* Place one CERT neuron per symbol that appears as a transition source.
     * Last teacher transition for that symbol wins (local Markov piece). */
    for (i = 0; i < L - 1; i++) {
        char name[64];
        int from = ids[i], to = ids[i + 1];
        snprintf(name, sizeof name, "neuron_%c", charset[from]);
        if (unit_exists(&reg, name)) {
            /* already placed — skip re-admit (same-name door) */
            continue;
        }
        if (admit_transition(&reg, &cov, pin, pout, name, A, from, to) != 0) {
            check(0, "admit neuron");
            goto done;
        }
        placed++;
    }
    check(placed >= 1, "CERT neurons placed");
    printf("  placed_neurons=%d alphabet=%d teacher_len=%d charset=\"%s\"\n", placed, A, L,
           charset);

    cnet_gen_engine_init(&chess, A, 2048);
    cnet_gen_engine_init(&resort, A, 2048);
    chess.in_port = pin;
    chess.out_port = pout;
    resort.in_port = pin;
    resort.out_port = pout;
    bind_all(&chess, &reg, charset, A);
    bind_all(&resort, &reg, charset, A);
    check(chess.n_bind >= 1, "chess bindings");
    check(resort.n_bind == chess.n_bind, "resort bindings match");

    start = ids[0];
    steps = STEPS;

    /* sample generation */
    {
        char out[256];
        size_t n = 0;
        CnetGenEngine one;
        cnet_gen_engine_init(&one, A, 512);
        one.in_port = pin;
        one.out_port = pout;
        bind_all(&one, &reg, charset, A);
        cnet_gen_run(&one, &cov, &reg, start, L - 1, 0, charset, out, sizeof out, &n);
        check(n > 0, "chess emits text");
        printf("  sample_chess_out=\"%s\" (len=%zu)\n", out, n);
        check(strchr(charset, out[0]) != NULL, "emit char in alphabet");
        cnet_gen_engine_free(&one);
    }

    /* timed experiment */
    t0 = wall_s();
    for (rep = 0; rep < REPS; rep++) {
        len_c = 0;
        fifo_clear(&chess.fifo);
        cnet_gen_run(&chess, &cov, &reg, start, steps, 0, charset, out_c, sizeof out_c, &len_c);
    }
    t1 = wall_s();
    chess_s = t1 - t0;
    fw_c = chess.n_forwards;

    t0 = wall_s();
    for (rep = 0; rep < REPS; rep++) {
        len_r = 0;
        fifo_clear(&resort.fifo);
        cnet_gen_run(&resort, &cov, &reg, start, steps, 1, charset, out_r, sizeof out_r, &len_r);
    }
    t1 = wall_s();
    resort_s = t1 - t0;
    fw_r = resort.n_forwards;

    printf("\n-- results --\n");
    printf("  reps=%d steps/rep=%d\n", REPS, steps);
    printf("  chess:  wall=%.4fs  forwards=%zu  emit=%zu  fetch=%zu  abstain=%zu\n", chess_s,
           fw_c, chess.n_emit, chess.n_fetch, chess.n_abstain);
    printf("  resort: wall=%.4fs  forwards=%zu  emit=%zu  resort_hits=%zu  abstain=%zu\n",
           resort_s, fw_r, resort.n_emit, resort.n_resort, resort.n_abstain);
    printf("  speedup_wall=%.2fx  forward_ratio_resort/chess=%.2fx\n",
           (chess_s > 1e-9) ? (resort_s / chess_s) : 0.0,
           (fw_c > 0) ? ((double)fw_r / (double)fw_c) : 0.0);
    printf("  chess_out_tail=\"%s\"\n", out_c);
    printf("  resort_out_tail=\"%s\"\n", out_r);

    check(chess.n_emit > 0, "chess emitted over bench");
    check(fw_c > 0 && fw_r > fw_c, "resort uses more forwards than chess");
    check((double)fw_r >= (double)fw_c * 1.5, "chess saves >=1.5x forwards vs re-sort");

    /* fail-closed: no bindings → abstain */
    {
        CnetGenEngine empty;
        char out[64];
        size_t n = 0;
        cnet_gen_engine_init(&empty, A, 64);
        empty.in_port = pin;
        empty.out_port = pout;
        cnet_gen_run(&empty, &cov, &reg, start, 10, 0, charset, out, sizeof out, &n);
        check(n == 0 && empty.n_abstain > 0, "no pieces → abstain (not parrot fill)");
        cnet_gen_engine_free(&empty);
    }

done:
    cnet_gen_engine_free(&chess);
    cnet_gen_engine_free(&resort);
    hybrid_ai_free(&cov);
    registry_free(&reg);

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("GENERATE_FIFO_PASS\n");
        return 0;
    }
    printf("GENERATE_FIFO_FAIL\n");
    return 1;
}
