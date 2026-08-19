/*
 * sevenseg_floor_study.c -- is the 7-segment confident-wrong CATCHABLE?
 *
 * The v4.4 habitat showed the 7-seg leaf has the largest accepted-symbol error
 * (~0.55 at the tail): symbols the margin gate accepted as CONFIDENT that are
 * nonetheless WRONG. The gate's whole premise is converting uncertainty into
 * abstention; on 7-seg at high noise it is passing confident-wrong instead.
 *
 * This study resolves the fork inside that number, with model-free instruments:
 *
 *   (1) nearest-clean-template error -- the best ANY classifier can do from
 *       these 7 features (the input-intrinsic floor). If the trained net's
 *       accepted-error tracks this floor, the error is in the INPUT (aliasing):
 *       the 7-seg code collapses digit pairs at noise, and nothing downstream
 *       can recover it -- it is the information-loss wall at the leaf input.
 *
 *   (2) ensemble agreement of K independently trained leaves on the
 *       accepted-WRONG cases. If the leaves DISAGREE, the error is model
 *       overconfidence -- catchable by a disagreement gate that is orthogonal
 *       to single-net margin. If they confidently AGREE on the same wrong
 *       digit, the collapse is in the features they all see -- uncatchable.
 *
 *   (3) a disagreement gate (accept only on unanimous argmax) measured for how
 *       much accepted-error it removes and at what coverage cost.
 *
 * Also prints the font's min Hamming distance per digit (the structural alias
 * prediction) and the dominant (true->wrong) confusion pairs.
 *
 * Standalone: links only src/nn.c. No router/contract/scan. Not in make test.
 */
#include "../include/nn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SEG7 7
#define KLEAVES 5

/* Exact font + renderer from the habitat (tests/glyph_habitat.c). */
static const unsigned char seg7_font[10][SEG7] = {
    {1,1,1,1,1,1,0}, {0,1,1,0,0,0,0}, {1,1,0,1,1,0,1}, {1,1,1,1,0,0,1},
    {0,1,1,0,0,1,1}, {1,0,1,1,0,1,1}, {1,0,1,1,1,1,1}, {1,1,1,0,0,0,0},
    {1,1,1,1,1,1,1}, {1,1,1,1,0,1,1},
};

static void render_noisy_7seg(int digit, double *feat, double noise) {
    int d = digit % 10;
    for (int s = 0; s < SEG7; s++) {
        double v = seg7_font[d][s] ? 0.9 : 0.1;
        v += (2.0 * ((double)rand() / RAND_MAX) - 1.0) * noise;
        if (v < 0.0) v = 0.0; if (v > 1.0) v = 1.0;
        feat[s] = v;
    }
}

/* argmax + (top1 - top2) margin over a length-10 normalized vector. */
static double max_margin(const double *p, int *best) {
    int b = 0; double m1 = -1, m2 = -1;
    for (int j = 0; j < 10; j++) if (p[j] > m1) { m2 = m1; m1 = p[j]; b = j; }
                                 else if (p[j] > m2) { m2 = p[j]; }
    *best = b; return m1 - m2;
}

/* leaf forward -> normalized 10-vector; returns argmax + margin. */
static int leaf_predict(BinaryTransformNetwork *net, const double *feat, double *margin) {
    const double *r = btn_forward(net, feat);
    double out[10], sum = 0;
    for (int j = 0; j < 10; j++) { out[j] = r[j]; sum += r[j]; }
    if (sum > 1e-6) for (int j = 0; j < 10; j++) out[j] /= sum;
    int b; *margin = max_margin(out, &b); return b;
}

/* model-free input floor: nearest clean template by L2 over the 7 raw features. */
static int nearest_template(const double *feat) {
    int best = 0; double bestd = 1e30;
    for (int d = 0; d < 10; d++) {
        double dist = 0;
        for (int s = 0; s < SEG7; s++) {
            double t = seg7_font[d][s] ? 0.9 : 0.1, e = feat[s] - t;
            dist += e * e;
        }
        if (dist < bestd) { bestd = dist; best = d; }
    }
    return best;
}

static void train_leaf(BinaryTransformNetwork *net, unsigned seed) {
    btn_init(net, SEG7, 10, 15, 15, 0.6, seed);
    enum { N = 2000 };
    static double in[N][SEG7], targ[N][10];
    srand(seed);  /* independent data draw per leaf */
    for (int i = 0; i < N; i++) {
        int d = i % 10;
        double n = 0.15 + 0.25 * ((double)rand() / RAND_MAX);
        render_noisy_7seg(d, in[i], n);
        for (int j = 0; j < 10; j++) targ[i][j] = (j == d ? 0.9 : 0.1);
    }
    btn_train_dynamic(net, &in[0][0], &targ[0][0], N, 300, 12, 0.01, 0.0008);
}

int main(int argc, char **argv) {
    unsigned run_seed = 0;
    if (argc > 1 && argv[1] && argv[1][0])
        run_seed = (unsigned)strtoul(argv[1], NULL, 10);
    else {
        const char *e = getenv("SEVENSEG_RUN_SEED");
        if (e && e[0]) run_seed = (unsigned)strtoul(e, NULL, 10);
    }

    printf("=== 7-segment confident-wrong: aliasing (input floor) vs overconfidence (catchable)? ===\n");
    printf("run_seed=%u (train leaf k uses 777+k+1000*run_seed; test srand=20260618+run_seed)\n",
           run_seed);

    /* structural alias prediction: min Hamming distance to any other digit */
    printf("\nfont min-Hamming-to-other-digit (1 => one segment apart => alias-prone):\n  ");
    for (int d = 0; d < 10; d++) {
        int mind = SEG7;
        for (int e = 0; e < 10; e++) if (e != d) {
            int h = 0; for (int s = 0; s < SEG7; s++) h += (seg7_font[d][s] != seg7_font[e][s]);
            if (h < mind) mind = h;
        }
        printf("%d:%d ", d, mind);
    }
    printf("\n");

    printf("\ntraining %d independent 7-seg leaves...\n", KLEAVES);
    static BinaryTransformNetwork leaf[KLEAVES];
    for (int k = 0; k < KLEAVES; k++)
        train_leaf(&leaf[k], 777u + (unsigned)k + 1000u * run_seed);

    const double floor = 0.15;            /* the habitat's recommended margin floor */
    const double noises[] = { 0.15, 0.30, 0.45, 0.60, 0.75 };
    const int n_noise = (int)(sizeof(noises) / sizeof(noises[0]));
    const int M = 600;                    /* samples per digit per noise */

    printf("\nfloor=%.2f, %d samples/digit. Leaf 0 is the 'deployed' leaf (matches habitat).\n", floor, M);
    printf("\nnoise | leaf0_acc_err | tmpl_floor_err | leaf0_cov | ens_agree_wrong | disagree_gate: resid_err  cov\n");

    srand(20260618u + run_seed);  /* test stream independent of training; varies by run_seed */
    for (int ni = 0; ni < n_noise; ni++) {
        double noise = noises[ni];
        long acc = 0, acc_wrong = 0;             /* leaf0 accepted / accepted-wrong */
        long tmpl_wrong = 0, tmpl_tot = 0;       /* model-free floor over all samples */
        long agree_wrong = 0;                    /* accepted-wrong where >=3/5 leaves give SAME wrong digit */
        long gate_acc = 0, gate_acc_wrong = 0;   /* unanimous-argmax disagreement gate */
        long conf[10][10] = {{0}};               /* leaf0 accepted-wrong (true -> pred) */

        for (int d = 0; d < 10; d++) {
            for (int s = 0; s < M; s++) {
                double feat[SEG7];
                render_noisy_7seg(d, feat, noise);

                /* (1) model-free input floor */
                tmpl_tot++;
                if (nearest_template(feat) != d) tmpl_wrong++;

                /* all K leaves */
                int pred[KLEAVES]; double mg[KLEAVES];
                for (int k = 0; k < KLEAVES; k++) pred[k] = leaf_predict(&leaf[k], feat, &mg[k]);

                int p0 = pred[0]; double m0 = mg[0];
                int accepted0 = (m0 >= floor);
                if (accepted0) {
                    acc++;
                    if (p0 != d) {
                        acc_wrong++;
                        conf[d][p0]++;
                        /* ensemble: how many leaves give leaf0's SAME wrong digit? */
                        int same = 0;
                        for (int k = 0; k < KLEAVES; k++) if (pred[k] == p0) same++;
                        if (same >= 3) agree_wrong++;   /* majority agrees on the wrong digit */
                    }
                }

                /* (3) disagreement gate: accept only if all K argmax agree (and leaf0 passes margin) */
                int unanimous = 1;
                for (int k = 1; k < KLEAVES; k++) if (pred[k] != p0) { unanimous = 0; break; }
                if (accepted0 && unanimous) {
                    gate_acc++;
                    if (p0 != d) gate_acc_wrong++;
                }
            }
        }

        double acc_err   = acc > 0 ? (double)acc_wrong / acc : 0.0;
        double tmpl_err  = (double)tmpl_wrong / tmpl_tot;
        double cov       = (double)acc / (tmpl_tot);
        double agree_frac= acc_wrong > 0 ? (double)agree_wrong / acc_wrong : 0.0;
        double gate_err  = gate_acc > 0 ? (double)gate_acc_wrong / gate_acc : 0.0;
        double gate_cov  = (double)gate_acc / tmpl_tot;

        printf("%.2f  |    %.4f     |    %.4f     |   %.3f   |      %.3f      |        %.4f   %.3f\n",
               noise, acc_err, tmpl_err, cov, agree_frac, gate_err, gate_cov);

        /* dominant confusions at the hardest cells */
        if (noise >= 0.60) {
            printf("       top leaf0 accepted-wrong confusions (true->pred): ");
            for (int r = 0; r < 3; r++) {
                int bi = -1, bj = -1; long bv = 0;
                for (int i = 0; i < 10; i++) for (int j = 0; j < 10; j++)
                    if (conf[i][j] > bv) { bv = conf[i][j]; bi = i; bj = j; }
                if (bv == 0) break;
                int h = 0; for (int s = 0; s < SEG7; s++) h += (seg7_font[bi][s] != seg7_font[bj][s]);
                printf("%d->%d (n=%ld, hamming=%d)  ", bi, bj, bv, h);
                conf[bi][bj] = 0;
            }
            printf("\n");
        }
    }

    printf("\nReading the table:\n");
    printf(" - leaf0_acc_err tracking tmpl_floor_err  => ALIASING (input below floor; uncatchable).\n");
    printf(" - leaf0_acc_err >> tmpl_floor_err        => OVERCONFIDENCE (net wrong on separable input).\n");
    printf(" - high ens_agree_wrong                   => leaves collapse identically => aliasing, gate can't help.\n");
    printf(" - disagree_gate resid_err << leaf0_acc_err => confident-wrong WAS catchable by ensemble.\n");

    for (int k = 0; k < KLEAVES; k++) btn_free(&leaf[k]);
    return 0;
}
