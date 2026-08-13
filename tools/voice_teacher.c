/* CNET external voice teacher — hermetic closed-set commands (core).
 *
 * Protocol (line-oriented):
 *   stdin:  IN <dim> d0 d1 ... d{dim-1}
 *   stdout: OUT <n_cmd> o0 o1 ... o{n_cmd-1}
 *
 * Whisper mode is WITHHELD in this C port (no ML deps).
 *
 * Usage:
 *   voice_teacher [--mode hermetic|whisper] [--check]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define FEAT_DIM 16
#define N_CMD 10

static const char *COMMANDS[N_CMD] = {
    "yes", "no", "up", "down", "left", "right", "on", "off", "stop", "go"
};

static float hermetic_table[N_CMD][FEAT_DIM];

static void hermetic_features(int class_id, float *out) {
    const char *name;
    uint32_t h = 2166136261u;
    size_t i;
    int k;
    if (class_id < 0 || class_id >= N_CMD) class_id = 0;
    name = COMMANDS[class_id];
    for (i = 0; name[i]; i++) {
        h ^= (uint8_t)name[i];
        h *= 16777619u;
    }
    h ^= (uint32_t)(class_id * 0x9E3779B9u);
    for (k = 0; k < FEAT_DIM; k++) {
        out[k] = ((h >> (k % 32)) & 1u) ? 0.85f : 0.15f;
        h = h * 1664525u + 1013904223u;
    }
}

static void init_table(void) {
    int c;
    for (c = 0; c < N_CMD; c++) hermetic_features(c, hermetic_table[c]);
}

static int hermetic_classify(const float *feat) {
    int best = 0, c, k;
    double best_d = 1e300;
    for (c = 0; c < N_CMD; c++) {
        double d = 0.0;
        for (k = 0; k < FEAT_DIM; k++) {
            double e = (double)feat[k] - (double)hermetic_table[c][k];
            d += e * e;
        }
        if (d < best_d) {
            best_d = d;
            best = c;
        }
    }
    return best;
}

static int serve(void) {
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        char *parts[FEAT_DIM + 8];
        int np = 0;
        char *p = line;
        float feat[FEAT_DIM];
        int dim, i, cls;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == '\n') continue;
        while (np < FEAT_DIM + 8 && *p && *p != '\n') {
            parts[np++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            if (*p) *p++ = 0;
            while (*p == ' ' || *p == '\t') p++;
        }
        if (np < 2 || strcmp(parts[0], "IN") != 0) {
            printf("ERR bad_line\n");
            fflush(stdout);
            continue;
        }
        dim = atoi(parts[1]);
        if (dim <= 0) {
            printf("ERR bad_dim\n");
            fflush(stdout);
            continue;
        }
        if (np - 2 < dim) {
            printf("ERR short_in\n");
            fflush(stdout);
            continue;
        }
        for (i = 0; i < FEAT_DIM; i++) feat[i] = 0.0f;
        for (i = 0; i < dim && i < FEAT_DIM; i++) feat[i] = (float)atof(parts[2 + i]);
        cls = hermetic_classify(feat);
        printf("OUT %d", N_CMD);
        for (i = 0; i < N_CMD; i++) printf(" %.17g", i == cls ? 1.0 : 0.0);
        printf("\n");
        fflush(stdout);
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = "hermetic";
    int check = 0, i;
    init_table();
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) mode = argv[++i];
        else if (strcmp(argv[i], "--check") == 0)
            check = 1;
        else if (strcmp(argv[i], "--label-wav") == 0) {
            fprintf(stderr, "label-wav WITHHELD (whisper mode not in C hermetic port)\n");
            return 2;
        }
    }
    if (strcmp(mode, "whisper") == 0) {
        if (check) {
            printf("mode=whisper n_cmd=%d feat_dim=%d\n", N_CMD, FEAT_DIM);
            printf("commands=");
            for (i = 0; i < N_CMD; i++) printf("%s%s", i ? "," : "", COMMANDS[i]);
            printf("\n");
            printf("whisper_unavailable: WITHHELD in C hermetic voice_teacher\n");
            return 1;
        }
        fprintf(stderr, "# voice_teacher whisper WITHHELD — falling back to hermetic\n");
        mode = "hermetic";
    }
    if (check) {
        printf("mode=%s n_cmd=%d feat_dim=%d\n", mode, N_CMD, FEAT_DIM);
        printf("commands=");
        for (i = 0; i < N_CMD; i++) printf("%s%s", i ? "," : "", COMMANDS[i]);
        printf("\n");
        printf("hermetic_ok\n");
        return 0;
    }
    return serve();
}
