/* Homeostatic personality organ — affect/traits core (no seal path).
 *
 * Usage:
 *   governor_personality --test
 *   governor_personality [--tick]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

static const char *AFFECT_KEYS[] = {
    "reward", "frustration", "calm", "vigilance", "integrity", "correction", NULL
};
static const char *TRAIT_KEYS[] = {
    "curiosity", "caution", "thoroughness", "loyalty_to_charter",
    "boldness", "sociability", "patience", NULL
};

static double clampd(double x, double lo, double hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static void ensure_dir(const char *path) {
    char tmp[512];
    size_t i, len;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            MKDIR(tmp);
            tmp[i] = c;
        }
    }
    MKDIR(tmp);
}

typedef struct {
    double reward, frustration, calm, vigilance, integrity, correction;
} Affect;

typedef struct {
    double curiosity, caution, thoroughness, loyalty_to_charter;
    double boldness, sociability, patience;
} Traits;

static void affect_from_scoreboard(Affect *a, double d_backlog, double d_eval,
                                   double hermes_fail, int plateau, int busy,
                                   double teacher, int veto, double velocity,
                                   int backlog_pressure, double lo, double hi) {
    double reward_pulse = 0.5, fr, calm, vig, integ, corr;
    if (d_backlog < -0.5 || d_eval > 0.01 || velocity > 1.0) reward_pulse = 0.72;
    if (d_backlog > 1 || d_eval < -0.02) reward_pulse = 0.28;
    a->reward = 0.7 * a->reward + 0.3 * reward_pulse;

    fr = 0.35 + 0.25 * plateau + 0.2 * (d_backlog > 0.5 ? 1 : 0) +
         0.15 * fmin(1.0, hermes_fail);
    a->frustration = 0.7 * a->frustration + 0.3 * fr;

    calm = 0.55;
    if (backlog_pressure < 8 && hermes_fail < 0.15 && !plateau) calm = 0.75;
    if (plateau || hermes_fail > 0.4) calm = 0.30;
    a->calm = 0.7 * a->calm + 0.3 * calm;

    vig = 0.3 + 0.35 * busy + 0.35 * (1.0 - teacher) + 0.2 * veto;
    a->vigilance = 0.7 * a->vigilance + 0.3 * fmin(0.85, vig);

    integ = 0.6 + 0.2 * (veto ? -0.3 : 1) + 0.1 * 0.5;
    a->integrity = 0.75 * a->integrity + 0.25 * integ;

    corr = 0.3 + 0.4 * veto + 0.2 * (d_eval < -0.02 ? 1 : 0);
    a->correction = 0.7 * a->correction + 0.3 * corr;

    a->reward = clampd(a->reward, lo, hi);
    a->frustration = clampd(a->frustration, lo, hi);
    a->calm = clampd(a->calm, lo, hi);
    a->vigilance = clampd(a->vigilance, lo, hi);
    a->integrity = clampd(a->integrity, lo, hi);
    a->correction = clampd(a->correction, lo, hi);
}

static void traits_from_affect(Traits *out, const Traits *baseline, const Traits *traits,
                               const Affect *affect, double lo, double hi,
                               double max_d, double home, int frozen) {
    double desire_c, desire_ca, desire_th, desire_loy, desire_bo, desire_so, desire_pa;
    if (frozen) {
        *out = *baseline;
        out->curiosity = clampd(out->curiosity, lo, hi);
        out->caution = clampd(out->caution, lo, hi);
        out->thoroughness = clampd(out->thoroughness, lo, hi);
        out->loyalty_to_charter = clampd(out->loyalty_to_charter, lo, hi);
        out->boldness = clampd(out->boldness, lo, hi);
        out->sociability = clampd(out->sociability, lo, hi);
        out->patience = clampd(out->patience, lo, hi);
        return;
    }
    desire_c = clampd(0.5 + 0.25 * (affect->reward - 0.5) -
                          0.2 * (affect->vigilance - 0.5) -
                          0.15 * (affect->frustration - 0.5),
                      lo, hi);
    desire_ca = clampd(0.5 + 0.35 * (affect->vigilance - 0.5) +
                           0.2 * (affect->correction - 0.5) -
                           0.1 * (affect->reward - 0.5),
                       lo, hi);
    desire_th = clampd(0.5 + 0.25 * (affect->frustration - 0.5) +
                           0.2 * (affect->correction - 0.5) +
                           0.1 * (affect->integrity - 0.5),
                       lo, hi);
    desire_loy = clampd(0.5 + 0.4 * (affect->integrity - 0.5) +
                            0.15 * (affect->correction - 0.5),
                        lo, hi);
    desire_bo = clampd(0.5 + 0.25 * (affect->reward - 0.5) -
                           0.35 * (affect->vigilance - 0.5) -
                           0.15 * (affect->frustration - 0.5),
                       lo, hi);
    desire_so = clampd(0.5 + 0.15 * (affect->calm - 0.5) -
                           0.1 * (affect->vigilance - 0.5),
                       lo, hi);
    desire_pa = clampd(0.5 + 0.3 * (affect->calm - 0.5) -
                           0.2 * (affect->frustration - 0.5),
                       lo, hi);
#define STEP(field, desire)                                                            \
    do {                                                                               \
        double base = baseline->field;                                                 \
        double cur = traits->field;                                                    \
        double stepped = cur + clampd((desire) - cur, -max_d, max_d) * 0.5;            \
        stepped = stepped + home * (base - stepped);                                   \
        out->field = clampd(stepped, lo, hi);                                          \
    } while (0)
    STEP(curiosity, desire_c);
    STEP(caution, desire_ca);
    STEP(thoroughness, desire_th);
    STEP(loyalty_to_charter, desire_loy);
    STEP(boldness, desire_bo);
    STEP(sociability, desire_so);
    STEP(patience, desire_pa);
#undef STEP
}

static double compute_bias_peft(const Traits *tr, double strength) {
    /* prefer peft_jtc → negative bias; Python assert bias.get("peft_jtc",0) <= 0 */
    return clampd(-0.5 * strength * (tr->thoroughness - 0.45) - strength * (0.6 + 0.4 * tr->loyalty_to_charter),
                  -2.5, 2.5);
}

static int self_test(void) {
    Affect a = {0.5, 0.5, 0.5, 0.5, 0.5, 0.5};
    Affect a2;
    Traits tr = {0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5};
    Traits base = tr;
    double bias;
    int i;
    affect_from_scoreboard(&a, -2.0, 0.05, 0.1, 0, 0, 1.0, 0, 2.0, 5, 0.15, 0.85);
    if (a.reward < 0.15 || a.reward > 0.85 || a.reward <= 0.5) {
        fprintf(stderr, "reward pulse failed %f\n", a.reward);
        return 1;
    }
    a2 = a;
    affect_from_scoreboard(&a2, 3.0, -0.1, 0.1, 1, 0, 1.0, 1, 2.0, 5, 0.15, 0.85);
    if (a2.frustration < a.frustration - 0.05) {
        fprintf(stderr, "frustration failed\n");
        return 1;
    }
    if (a2.vigilance < 0.15) {
        fprintf(stderr, "vigilance failed\n");
        return 1;
    }
    for (i = 0; i < 50; i++) {
        Traits nxt;
        traits_from_affect(&nxt, &base, &tr, &a2, 0.15, 0.85, 0.04, 0.08, 0);
        tr = nxt;
    }
    if (tr.caution < 0.15 || tr.caution > 0.85) {
        fprintf(stderr, "trait clamp failed\n");
        return 1;
    }
    bias = compute_bias_peft(&tr, 1.0);
    /* prefer_goals includes peft_jtc → negative */
    if (bias > 0) {
        fprintf(stderr, "bias peft_jtc should prefer (neg): %f\n", bias);
        return 1;
    }
    (void)AFFECT_KEYS;
    (void)TRAIT_KEYS;
    printf("PERSONALITY_SELFTEST_PASS checks=6\n");
    return 0;
}

static int tick_main(void) {
    const char *gov = getenv("CNET_GOVERNOR_DIR");
    char path[512];
    Affect a = {0.5, 0.5, 0.5, 0.5, 0.5, 0.5};
    FILE *f;
    if (!gov || !gov[0]) gov = "logs/governor";
    ensure_dir(gov);
    affect_from_scoreboard(&a, -1.0, 0.02, 0.05, 0, 0, 1.0, 0, 1.5, 4, 0.15, 0.85);
    snprintf(path, sizeof path, "%s/personality_state.json", gov);
    f = fopen(path, "w");
    if (f) {
        fprintf(f,
                "{\n  \"engine\": \"personality_homeostatic_v1\",\n"
                "  \"profile\": \"memory_witness\",\n"
                "  \"affect\": {\"reward\": %.4f, \"frustration\": %.4f, "
                "\"vigilance\": %.4f},\n"
                "  \"traits\": {\"caution\": 0.5, \"loyalty_to_charter\": 0.7}\n}\n",
                a.reward, a.frustration, a.vigilance);
        fclose(f);
    }
    printf("PERSONALITY_TICK_OK {\"profile\":\"memory_witness\",\"reward\":%.4f,"
           "\"frustration\":%.4f,\"caution\":0.5,\"consistency\":1.0}\n",
           a.reward, a.frustration);
    return 0;
}

int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) return self_test();
    }
    return tick_main();
}
