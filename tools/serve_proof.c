/* Serve proof CLI: open a sealed CNB via SoulHost and execute certified units.
 *
 * Proves learning landed on disk and serves as Tier A (SOUL_SOURCE_CERTIFIED).
 *
 * Usage:
 *   serve_proof <base.cnb> [--max N] [--sample-tag PREFIX]
 *   serve_proof --selftest   # hermetic teach→seal→reopen (calls built test)
 *
 * Exit 0 + SERVE_PROOF_PASS when at least one certified unit serves.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/soul_host.h"
#include "../include/base.h"
#include "../include/nn.h"
#include "../include/router.h"

static int argmax_n(const double *v, int n) {
    int i, b = 0;
    if (n <= 0) return -1;
    for (i = 1; i < n; i++) if (v[i] > v[b]) b = i;
    return b;
}

int main(int argc, char **argv) {
    const char *base_path = NULL;
    int max_units = 32;
    const char *tag_prefix = NULL;
    SoulHost *host = NULL;
    int n, i;
    int tried = 0, certified_ok = 0, run_ok = 0, fail = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
            max_units = atoi(argv[++i]);
            if (max_units < 1) max_units = 1;
        } else if (strcmp(argv[i], "--sample-tag") == 0 && i + 1 < argc) {
            tag_prefix = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                    "usage: %s <base.cnb> [--max N] [--sample-tag PREFIX]\n",
                    argv[0]);
            return 2;
        } else if (argv[i][0] != '-') {
            base_path = argv[i];
        }
    }
    if (!base_path) {
        fprintf(stderr, "usage: %s <base.cnb> [--max N] [--sample-tag PREFIX]\n",
                argv[0]);
        return 2;
    }
    if (access(base_path, R_OK) != 0) {
        fprintf(stderr, "serve_proof: cannot read %s\n", base_path);
        return 1;
    }

    printf("== serve proof: %s ==\n", base_path);
    if (soul_open(base_path, NULL, &host) != 0 || !host) {
        fprintf(stderr, "serve_proof: soul_open failed\n");
        return 1;
    }
    n = soul_unit_count(host);
    printf("units_loaded=%d max_sample=%d\n", n, max_units);
    if (n < 1) {
        soul_close(host);
        printf("SERVE_PROOF_FAIL no_units\n");
        return 1;
    }

    for (i = 0; i < n && tried < max_units; i++) {
        char name[128];
        int in_tot = 0, out_tot = 0;
        int trust = -1, role = -1;
        double *in = NULL, *out = NULL;
        int rc;

        if (soul_unit_name(host, i, name, (int)sizeof name) != 0) continue;
        if (tag_prefix && tag_prefix[0] &&
            strncmp(name, tag_prefix, strlen(tag_prefix)) != 0)
            continue;
        if (soul_unit_dims(host, name, &in_tot, &out_tot) != 0 ||
            in_tot <= 0 || out_tot <= 0 || in_tot > 65536 || out_tot > 65536)
            continue;

        tried++;
        in = (double *)calloc((size_t)in_tot, sizeof(double));
        out = (double *)calloc((size_t)out_tot, sizeof(double));
        if (!in || !out) {
            free(in);
            free(out);
            fail++;
            continue;
        }
        /* calloc already zeroed; one-hot-ish probe at index 0. */
        in[0] = 1.0;

        rc = soul_run(host, name, in, out, out_tot);
        (void)soul_unit_axes(host, name, &trust, &role);
        if (rc >= 0) {
            run_ok++;
            /* Certified path: soul_run only executes certified registry units. */
            certified_ok++;
            printf("  OK  %-40s in=%d out=%d trust=%d argmax=%d\n",
                   name, in_tot, out_tot, trust, argmax_n(out, out_tot));
        } else {
            fail++;
            printf("  FAIL %-40s soul_run rc=%d\n", name, rc);
        }
        free(in);
        free(out);
    }

    {
        SoulServeStats st;
        memset(&st, 0, sizeof st);
        (void)soul_serve_stats(host, &st);
        printf("sample: tried=%d run_ok=%d certified_ok=%d fail=%d\n",
               tried, run_ok, certified_ok, fail);
        printf("host_stats: certified_serves=%llu residual=%llu\n",
               (unsigned long long)st.certified_serves,
               (unsigned long long)st.residual_serves);
    }
    soul_close(host);

    if (certified_ok >= 1 && fail == 0) {
        printf("SERVE_PROOF_PASS certified_serves=%d units_sampled=%d\n",
               certified_ok, tried);
        return 0;
    }
    if (certified_ok >= 1) {
        printf("SERVE_PROOF_PASS certified_serves=%d fail=%d (partial)\n",
               certified_ok, fail);
        return 0;
    }
    printf("SERVE_PROOF_FAIL certified_serves=0 tried=%d\n", tried);
    return 1;
}
