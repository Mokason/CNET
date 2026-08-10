/* Hermetic gate: load each daily pack SEPARATELY and measure local hit-rate.
 * make roe_daily_packs → ROE_DAILY_PACKS_PASS
 *
 * Token thesis: small per-pack catalogs beat one monobrain soup on repeat queries.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../include/cnet_roe_asi.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-64s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int is_dir(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int load_queries(const char *path, char ***out_q, int *out_n) {
    FILE *f;
    char line[ROE_TEXT_MAX];
    char **q = NULL;
    int n = 0, cap = 0;
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        char *s;
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (!L) continue;
        if (n + 1 > cap) {
            int ncap = cap ? cap * 2 : 16;
            char **nq = (char **)realloc(q, (size_t)ncap * sizeof(char *));
            if (!nq) {
                fclose(f);
                return -2;
            }
            q = nq;
            cap = ncap;
        }
        s = (char *)malloc(L + 1);
        if (!s) {
            fclose(f);
            return -3;
        }
        memcpy(s, line, L + 1);
        q[n++] = s;
    }
    fclose(f);
    *out_q = q;
    *out_n = n;
    return 0;
}

static void free_queries(char **q, int n) {
    int i;
    if (!q) return;
    for (i = 0; i < n; i++) free(q[i]);
    free(q);
}

static int path_join2(char *out, size_t cap, const char *a, const char *b) {
    size_t na, nb;
    if (!out || !cap || !a || !b) return -1;
    na = strlen(a);
    nb = strlen(b);
    if (na + 1 + nb + 1 > cap) return -1;
    memcpy(out, a, na);
    out[na] = '/';
    memcpy(out + na + 1, b, nb);
    out[na + 1 + nb] = 0;
    return 0;
}

static int pack_has_abi(const char *dir) {
    char path[ROE_PATH_MAX];
    FILE *f;
    char line[128];
    int ok = 0;
    if (path_join2(path, sizeof path, dir, "PACK.abi") != 0) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    if (fgets(line, sizeof line, f) && strstr(line, "ROE_DAILY_PACK")) ok = 1;
    fclose(f);
    return ok;
}

typedef struct {
    char id[64];
    int n_skills;
    int n_q;
    double hit;
    double save;
    int ood_abstain_ok;
} PackResult;

static int run_pack(const char *dir, PackResult *pr) {
    RoeAsi R;
    RoeTrainReport tr;
    char **queries = NULL;
    int nq = 0, i;
    char qpath[ROE_PATH_MAX], stats[700];
    const char *batch[128];
    int nb = 0;
    RoeReply rep;

    memset(pr, 0, sizeof *pr);
    {
        const char *slash = strrchr(dir, '/');
        snprintf(pr->id, sizeof pr->id, "%s", slash ? slash + 1 : dir);
    }

    roe_init(&R);
    roe_set_catalog_dir(&R, dir);
    pr->n_skills = roe_load_catalog(&R);
    if (pr->n_skills <= 0) return -1;

    snprintf(qpath, sizeof qpath, "%s", dir);
    if (path_join2(qpath, sizeof qpath, dir, "queries_train.txt") != 0) {
        free_queries(queries, nq);
        return -2;
    }
    if (load_queries(qpath, &queries, &nq) != 0 || nq <= 0) {
        free_queries(queries, nq);
        return -2;
    }
    pr->n_q = nq;
    for (i = 0; i < nq && nb < 128; i++) batch[nb++] = queries[i];

    /* 2 epochs: promote nothing new (all day-0 CERT); measure hit */
    for (i = 0; i < 2; i++) {
        roe_reset_stats(&R);
        roe_train_epoch(&R, batch, nb, &tr);
    }
    pr->hit = tr.local_hit_rate;
    pr->save = tr.token_save_ratio;
    roe_dump_stats(&R, stats, sizeof stats);

    /* last query often OOD zz — expect non-LOCAL or abstain/ask */
    roe_turn(&R, queries[nq - 1], &rep);
    pr->ood_abstain_ok =
        (rep.source != ROE_SRC_LOCAL) || (rep.verified == 0 && rep.skill_id[0] == 0);

    printf("  pack %-28s skills=%d q=%d hit=%.1f%% save=%.1f%% ood_src=%s\n",
           pr->id, pr->n_skills, pr->n_q, 100.0 * pr->hit, 100.0 * pr->save,
           roe_source_name(rep.source));
    printf("    %s\n", stats);

    free_queries(queries, nq);
    return 0;
}

int main(void) {
    const char *root = "artifacts/roe_daily_packs";
    DIR *d;
    struct dirent *de;
    PackResult results[32];
    int nres = 0;
    double hit_sum = 0.0;
    int i;
    FILE *idx;

    failures = checks = 0;
    printf("=== ROE daily packs (separate load) ===\n");

    check(is_dir(root), "packs root exists (run seed)");

    d = opendir(root);
    check(d != NULL, "opendir packs root");
    if (!d) {
        printf("ROE_DAILY_PACKS_FAIL\n");
        return 1;
    }

    while ((de = readdir(d)) != NULL) {
        char path[ROE_PATH_MAX];
        PackResult pr;
        size_t nl, nr;
        if (de->d_name[0] == '.') continue;
        if (strncmp(de->d_name, "pack_", 5) != 0) continue;
        nl = strlen(root);
        nr = strlen(de->d_name);
        if (nl + 1 + nr + 1 > sizeof path) continue;
        memcpy(path, root, nl);
        path[nl] = '/';
        memcpy(path + nl + 1, de->d_name, nr);
        path[nl + 1 + nr] = 0;
        if (!is_dir(path) || !pack_has_abi(path)) continue;
        if (run_pack(path, &pr) != 0) {
            check(0, path);
            continue;
        }
        check(pr.n_skills >= 4, pr.id);
        /* Repeat queries in train list → expect solid local hit */
        check(pr.hit >= 0.45, "local hit >= 45% on pack queries");
        check(pr.save >= 0.40, "token save >= 40% vs all-teacher baseline");
        if (nres < 32) {
            results[nres++] = pr;
            hit_sum += pr.hit;
        }
    }
    closedir(d);

    check(nres >= 10, "at least 10 separate packs loaded");
    if (nres > 0) {
        double mean = hit_sum / (double)nres;
        printf("  mean_hit_across_packs=%.1f%% n=%d\n", 100.0 * mean, nres);
        check(mean >= 0.50, "mean hit across packs >= 50%");
    }

    /* INDEX.json present */
    idx = fopen("artifacts/roe_daily_packs/INDEX.json", "r");
    check(idx != NULL, "INDEX.json present");
    if (idx) fclose(idx);
    idx = fopen("artifacts/roe_daily_packs/ROUTES.jsonl", "r");
    check(idx != NULL, "ROUTES.jsonl present");
    if (idx) fclose(idx);
    idx = fopen("artifacts/roe_daily_packs/MISS_LOG.schema.json", "r");
    check(idx != NULL, "MISS_LOG.schema.json present");
    if (idx) fclose(idx);

    /* write bench summary */
    {
        FILE *f = fopen("artifacts/roe_daily_packs/BENCH.json", "w");
        if (f) {
            fprintf(f, "{\n  \"n_packs\": %d,\n  \"packs\": [\n", nres);
            for (i = 0; i < nres; i++) {
                fprintf(f,
                        "    {\"id\":\"%s\",\"n_skills\":%d,\"hit\":%.4f,\"save\":%.4f}%s\n",
                        results[i].id, results[i].n_skills, results[i].hit,
                        results[i].save, (i + 1 < nres) ? "," : "");
            }
            fprintf(f, "  ],\n  \"never_self_cert\": true,\n  \"second_brain\": false\n}\n");
            fclose(f);
        }
    }

    printf("\nchecks=%d failures=%d packs=%d\n", checks, failures, nres);
    if (failures) {
        printf("ROE_DAILY_PACKS_FAIL\n");
        return 1;
    }
    printf("ROE_DAILY_PACKS_PASS\n");
    return 0;
}
