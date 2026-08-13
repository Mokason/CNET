/* Analyze a CNET_MARGIN_SWEEP TSV: choose the v2 yield lever.
 *
 * Usage:
 *   margin_sweep_analyze <sweep.tsv> [--ledger PATH] [--eps 0.02]
 *     [--evidence 0.9] [--min-evidence 16] [--semantics set|ordered]
 *     [--export-minable PATH] [--export-unfit PATH]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_UNITS 4096
#define MAX_PROBES 4096

typedef struct {
    int token;
    int n;
    float ordered[MAX_PROBES];
    float setm[MAX_PROBES];
} Unit;

static Unit units[MAX_UNITS];
static int n_units;
static char header[1024];

static Unit *find_or_add(int tok) {
    int i;
    for (i = 0; i < n_units; i++)
        if (units[i].token == tok) return &units[i];
    if (n_units >= MAX_UNITS) return NULL;
    units[n_units].token = tok;
    units[n_units].n = 0;
    return &units[n_units++];
}

static int read_sweep(const char *path) {
    FILE *f = fopen(path, "r");
    char line[1024];
    if (!f) return -1;
    header[0] = 0;
    n_units = 0;
    while (fgets(line, sizeof line, f)) {
        int unit_idx, token, probe_idx, rank;
        float ord, setv;
        Unit *u;
        if (line[0] == '#') {
            snprintf(header, sizeof header, "%s", line);
            { size_t n = strlen(header); while (n && (header[n-1]=='\n'||header[n-1]=='\r')) header[--n]=0; }
            continue;
        }
        if (strncmp(line, "unit_idx", 8) == 0) continue;
        if (sscanf(line, "%d %d %d %d %f %f", &unit_idx, &token, &probe_idx, &rank, &ord, &setv) != 6)
            continue;
        (void)unit_idx; (void)probe_idx; (void)rank;
        u = find_or_add(token);
        if (!u || u->n >= MAX_PROBES) continue;
        u->ordered[u->n] = ord;
        u->setm[u->n] = setv;
        u->n++;
    }
    fclose(f);
    return n_units > 0 ? 0 : -1;
}

static const char *gate(const Unit *u, int idx, float eps, float evidence, int min_evidence,
                        int *usable_out) {
    int i, usable = 0;
    for (i = 0; i < u->n; i++) {
        float m = (idx == 0) ? u->ordered[i] : u->setm[i];
        if (m >= eps) usable++;
    }
    *usable_out = usable;
    if (usable == 0) return "oracle_unfit";
    if (usable < min_evidence) return "insufficient_exemplars";
    if ((float)usable / (float)u->n < evidence) return "oracle_unfit";
    return "minable";
}

typedef struct {
    int token;
    char actual[64];
} Led;

static int extract_tk(const char *line, int *tok) {
    const char *p = line;
    while ((p = strstr(p, "tk")) != NULL) {
        const char *q = p + 2;
        int a = 0;
        if (!isdigit((unsigned char)*q)) { p++; continue; }
        while (isdigit((unsigned char)*q)) a = a * 10 + (*q++ - '0');
        if (*q == 'q' && isdigit((unsigned char)q[1])) {
            *tok = a;
            return 1;
        }
        p++;
    }
    return 0;
}

static int read_ledger(const char *path, Led *led, int maxn) {
    FILE *f = fopen(path, "r");
    char line[4096];
    int n = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f) && n < maxn) {
        int tok;
        size_t L;
        if (!extract_tk(line, &tok)) continue;
        led[n].token = tok;
        L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        if (strstr(line, " acq_") || (L >= 2 && line[L-1] == '-' && line[L-2] == ' ')) {
            snprintf(led[n].actual, sizeof led[n].actual, "acquired");
        } else {
            const char *reasons[] = {
                "oracle_unfit", "insufficient_exemplars", "certify_failed",
                "class_imbalance", "unbounded_domain", "incumbent_healthy", NULL
            };
            int r;
            snprintf(led[n].actual, sizeof led[n].actual, "deferred");
            for (r = 0; reasons[r]; r++)
                if (strstr(line, reasons[r])) {
                    snprintf(led[n].actual, sizeof led[n].actual, "%s", reasons[r]);
                    break;
                }
        }
        n++;
    }
    fclose(f);
    return n;
}

static const char *led_get(Led *led, int n, int tok) {
    int i;
    for (i = 0; i < n; i++)
        if (led[i].token == tok) return led[i].actual;
    return NULL;
}

typedef struct {
    int token;
    const char *vo, *vs;
    int uo, us;
    float mean_set;
} Row;

static int cmp_token(const void *a, const void *b) {
    return ((const Row *)a)->token - ((const Row *)b)->token;
}

static int cmp_mean_desc(const void *a, const void *b) {
    float da = ((const Row *)a)->mean_set, db = ((const Row *)b)->mean_set;
    if (db > da) return 1;
    if (db < da) return -1;
    return 0;
}

int main(int argc, char **argv) {
    const char *tsv = NULL, *ledger = NULL;
    const char *export_minable = NULL, *export_unfit = NULL;
    const char *semantics = "set";
    float eps = 0.02f, evidence = 0.9f;
    int min_evidence = 16;
    int ai, i, full, nu;
    float grid[16];
    int ngrid = 0;
    Row *rows;
    int n_rows;

    for (ai = 1; ai < argc; ai++) {
        if (strcmp(argv[ai], "--ledger") == 0 && ai + 1 < argc) ledger = argv[++ai];
        else if (strcmp(argv[ai], "--eps") == 0 && ai + 1 < argc) eps = (float)atof(argv[++ai]);
        else if (strcmp(argv[ai], "--evidence") == 0 && ai + 1 < argc) evidence = (float)atof(argv[++ai]);
        else if (strcmp(argv[ai], "--min-evidence") == 0 && ai + 1 < argc) min_evidence = atoi(argv[++ai]);
        else if (strcmp(argv[ai], "--semantics") == 0 && ai + 1 < argc) semantics = argv[++ai];
        else if (strcmp(argv[ai], "--export-minable") == 0 && ai + 1 < argc) export_minable = argv[++ai];
        else if (strcmp(argv[ai], "--export-unfit") == 0 && ai + 1 < argc) export_unfit = argv[++ai];
        else if (!tsv) tsv = argv[ai];
        else {
            fprintf(stderr, "usage: %s <sweep.tsv> [options]\n", argv[0]);
            return 2;
        }
    }
    if (!tsv) {
        fprintf(stderr, "usage: %s <sweep.tsv> [options]\n", argv[0]);
        return 2;
    }
    if (read_sweep(tsv) != 0) {
        fprintf(stderr, "no sweep rows in %s\n", tsv);
        return 1;
    }
    printf("%s\n", header[0] ? header : "(no header)");

    full = 0;
    for (i = 0; i < n_units; i++)
        if (units[i].n > full) full = units[i].n;
    {
        int w = 0;
        for (i = 0; i < n_units; ) {
            if (units[i].n != full) {
                if (w < 8) {
                    if (w == 0) printf("WARNING: dropped partial unit(s)");
                    printf("%s tk%d", w ? "," : ":", units[i].token);
                    w++;
                }
                units[i] = units[--n_units];
            } else i++;
        }
        if (w) printf(" (< %d probes)\n", full);
    }
    if (!n_units) {
        fprintf(stderr, "no complete units\n");
        return 1;
    }
    nu = n_units;
    printf("units=%d probes/unit=%d\n", nu, full);

    {
        float base[] = {0.005f, 0.01f, 0.02f, 0.03f, 0.05f, 0.08f, 0.12f};
        int has = 0;
        for (i = 0; i < 7; i++) grid[ngrid++] = base[i];
        for (i = 0; i < ngrid; i++) if (fabsf(grid[i] - eps) < 1e-12f) has = 1;
        if (!has) {
            grid[ngrid++] = eps;
            /* sort */
            {
                int a, b;
                for (a = 0; a < ngrid; a++)
                    for (b = a + 1; b < ngrid; b++)
                        if (grid[b] < grid[a]) {
                            float t = grid[a]; grid[a] = grid[b]; grid[b] = t;
                        }
            }
        }
    }

    printf("\npredicted minable units (evidence>=%.2f, min_evidence=%d):\n",
           evidence, min_evidence);
    printf("%-8s %12s %12s %10s\n", "eps", "ordered", "set", "recovered");
    for (i = 0; i < ngrid; i++) {
        int yo = 0, ys = 0, u;
        float e = grid[i];
        for (u = 0; u < n_units; u++) {
            int uu;
            if (strcmp(gate(&units[u], 0, e, evidence, min_evidence, &uu), "minable") == 0) yo++;
            if (strcmp(gate(&units[u], 1, e, evidence, min_evidence, &uu), "minable") == 0) ys++;
        }
        printf("%-8g %8d/%-3d %8d/%-3d %+10d%s\n",
               e, yo, nu, ys, nu, ys - yo,
               fabsf(e - eps) < 1e-12f ? "  <-- campaign eps" : "");
    }

    rows = (Row *)calloc((size_t)n_units, sizeof(Row));
    n_rows = n_units;
    for (i = 0; i < n_units; i++) {
        int j;
        float sum = 0;
        rows[i].token = units[i].token;
        rows[i].vo = gate(&units[i], 0, eps, evidence, min_evidence, &rows[i].uo);
        rows[i].vs = gate(&units[i], 1, eps, evidence, min_evidence, &rows[i].us);
        for (j = 0; j < units[i].n; j++) sum += units[i].setm[j];
        rows[i].mean_set = sum / (float)units[i].n;
    }
    qsort(rows, (size_t)n_rows, sizeof(Row), cmp_token);

    {
        int conv = 0, lost = 0;
        for (i = 0; i < n_rows; i++) {
            if (strcmp(rows[i].vo, "minable") != 0 && strcmp(rows[i].vs, "minable") == 0) conv++;
            if (strcmp(rows[i].vo, "minable") == 0 && strcmp(rows[i].vs, "minable") != 0) lost++;
        }
        printf("\nat eps=%g: set-semantics converts %d units, loses %d\n", eps, conv, lost);
    }

    {
        int nmin = 0, nunfit = 0;
        int use_set = strcmp(semantics, "set") == 0;
        for (i = 0; i < n_rows; i++) {
            const char *v = use_set ? rows[i].vs : rows[i].vo;
            if (strcmp(v, "minable") == 0) nmin++;
            else nunfit++;
        }
        printf("\nexport semantics=%s @ eps=%g: minable=%d unfit=%d\n",
               semantics, eps, nmin, nunfit);
        if (export_minable) {
            FILE *f = fopen(export_minable, "w");
            if (f) {
                fprintf(f, "# predicted-minable token ids (%s eps=%g evidence>=%.2f min_ev=%d)\n",
                        semantics, eps, evidence, min_evidence);
                for (i = 0; i < n_rows; i++) {
                    const char *v = use_set ? rows[i].vs : rows[i].vo;
                    if (strcmp(v, "minable") == 0) fprintf(f, "%d\n", rows[i].token);
                }
                fclose(f);
                printf("wrote allowlist %s (%d tokens)\n", export_minable, nmin);
            }
        }
        if (export_unfit) {
            FILE *f = fopen(export_unfit, "w");
            if (f) {
                fprintf(f, "# predicted-unfit token ids (%s eps=%g)\n", semantics, eps);
                for (i = 0; i < n_rows; i++) {
                    const char *v = use_set ? rows[i].vs : rows[i].vo;
                    if (strcmp(v, "minable") != 0) fprintf(f, "%d\n", rows[i].token);
                }
                fclose(f);
                printf("wrote unfit list %s (%d tokens)\n", export_unfit, nunfit);
            }
        }
    }

    qsort(rows, (size_t)n_rows, sizeof(Row), cmp_mean_desc);
    printf("\ntop 10 / bottom 10 by mean set margin (screen decisiveness):\n");
    for (i = 0; i < n_rows && i < 10; i++)
        printf("  tk%-7d mean_set=%8.4f ordered=%s set=%s\n",
               rows[i].token, rows[i].mean_set, rows[i].vo, rows[i].vs);
    printf("  ...\n");
    for (i = (n_rows > 10 ? n_rows - 10 : 0); i < n_rows; i++)
        printf("  tk%-7d mean_set=%8.4f ordered=%s set=%s\n",
               rows[i].token, rows[i].mean_set, rows[i].vo, rows[i].vs);

    if (ledger) {
        Led led[8192];
        int nled = read_ledger(ledger, led, 8192);
        int tp = 0, fp = 0, fn = 0, tn = 0;
        printf("\nvalidation vs ledger %s (ordered @ eps=%g):\n", ledger, eps);
        for (i = 0; i < n_rows; i++) {
            const char *actual = led_get(led, nled, rows[i].token);
            int pred_min, act_min;
            if (!actual) continue;
            pred_min = strcmp(rows[i].vo, "minable") == 0;
            act_min = strcmp(actual, "acquired") == 0;
            if (pred_min && act_min) tp++;
            else if (pred_min && !act_min) {
                fp++;
                if (fp <= 12)
                    printf("    fp tk%d actual=%s usable=%d\n",
                           rows[i].token, actual, rows[i].uo);
            } else if (!pred_min && act_min) {
                fn++;
                if (fn <= 12)
                    printf("    fn tk%d usable=%d\n", rows[i].token, rows[i].uo);
            } else tn++;
        }
        printf("  predicted-minable & acquired:      %3d (true positive)\n", tp);
        printf("  predicted-minable & deferred:      %3d (downstream defers)\n", fp);
        printf("  predicted-unfit   & acquired:      %3d (should be ~0)\n", fn);
        printf("  predicted-unfit   & deferred:      %3d (true negative)\n", tn);
    }

    free(rows);
    return 0;
}
