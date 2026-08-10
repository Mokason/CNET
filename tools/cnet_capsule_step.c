/* cnet_capsule_step — light runtime: load a *CNET capsule package* and forward.
 *
 * Capsule dir = unit.cnb (CNB1 container) + manifest.cknow (from cnet_capsule_export).
 * Uses cnet_capsule_import (floors/seals intact) then cnb_get_unit + btn_forward.
 *
 * This is the cheap host path: run what main CNET certified, no MCP/.NET.
 *
 *   cnet_capsule_step <capsule_dir> --info
 *   cnet_capsule_step <capsule_dir> --x 0,1,0,... [--out path]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/base.h"
#include "../include/cnet_capsule.h"
#include "../include/contract/contract.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"

static void usage(const char *a0) {
    fprintf(stderr,
            "usage:\n"
            "  %s <capsule_dir> --info\n"
            "  %s <capsule_dir> --x v0,v1,... [--out path]\n"
            "  %s <capsule_dir> --xfile path [--out path]\n",
            a0, a0, a0);
}

static int parse_x(const char *s, double *x, int maxn, int *n_out) {
    int n = 0;
    const char *p = s;
    while (*p && n < maxn) {
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p) break;
        x[n++] = v;
        p = end;
        if (*p == ',') p++;
    }
    *n_out = n;
    return n > 0 ? 0 : -1;
}

static int read_xfile(const char *path, double *x, int maxn, int *n_out) {
    FILE *f = fopen(path, "rb");
    char buf[16384];
    size_t n, i;
    if (!f) return -1;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    for (i = 0; i < n; i++)
        if (buf[i] == '\n' || buf[i] == ' ' || buf[i] == '\t') buf[i] = ',';
    return parse_x(buf, x, maxn, n_out);
}

int main(int argc, char **argv) {
    const char *dir = NULL;
    const char *xs = NULL;
    const char *xfile = NULL;
    const char *outpath = NULL;
    int info = 0, i;
    CnetBase base;
    HybridAi cov;
    CnetCapsuleReport rep;
    BinaryTransformNetwork btn;
    Contract c;
    const char *unit_name;

    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }
    dir = argv[1];
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--info") == 0) info = 1;
        else if (strcmp(argv[i], "--x") == 0 && i + 1 < argc) xs = argv[++i];
        else if (strcmp(argv[i], "--xfile") == 0 && i + 1 < argc) xfile = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) outpath = argv[++i];
    }

    cnb_init(&base);
    hybrid_ai_init(&cov);
    memset(&rep, 0, sizeof rep);
    if (cnet_capsule_import(&base, &cov, dir, &rep) != 0) {
        fprintf(stderr, "cnet_capsule_import failed: %s\n", rep.reject_reason);
        cnb_free(&base);
        hybrid_ai_free(&cov);
        return 1;
    }
    unit_name = rep.unit[0] ? rep.unit : NULL;
    if (!unit_name || !cnb_has_unit(&base, unit_name)) {
        fprintf(stderr, "import ok but unit missing\n");
        cnb_free(&base);
        hybrid_ai_free(&cov);
        return 1;
    }

    memset(&btn, 0, sizeof btn);
    memset(&c, 0, sizeof c);
    if (cnb_get_unit(&base, unit_name, &btn, &c) != 0) {
        fprintf(stderr, "cnb_get_unit failed for %s\n", unit_name);
        cnb_free(&base);
        hybrid_ai_free(&cov);
        return 1;
    }

    if (info) {
        printf("unit=%s seal_verified=%d\n", c.name, c.seal_verified);
        printf("behavior_digest=%llu\n", (unsigned long long)rep.behavior_digest);
        printf("in=%zu out=%zu hidden=%zu\n", btn.input_count, btn.output_count, btn.hidden_count);
        printf("exemplars=%zu coverage_rows=%zu payload_bytes=%zu\n", rep.exemplars,
               rep.coverage_rows, rep.payload_bytes);
        contract_free(&c);
        btn_free(&btn);
        cnb_free(&base);
        hybrid_ai_free(&cov);
        return 0;
    }

    if (!xs && !xfile) {
        usage(argv[0]);
        contract_free(&c);
        btn_free(&btn);
        cnb_free(&base);
        hybrid_ai_free(&cov);
        return 2;
    }

    {
        double *x = (double *)calloc(btn.input_count + 8, sizeof(double));
        int nx = 0;
        const double *y;
        if (!x) {
            contract_free(&c);
            btn_free(&btn);
            cnb_free(&base);
            hybrid_ai_free(&cov);
            return 1;
        }
        if (xs) {
            if (parse_x(xs, x, (int)btn.input_count, &nx) != 0) {
                fprintf(stderr, "bad --x\n");
                free(x);
                contract_free(&c);
                btn_free(&btn);
                cnb_free(&base);
                hybrid_ai_free(&cov);
                return 1;
            }
        } else if (read_xfile(xfile, x, (int)btn.input_count, &nx) != 0) {
            fprintf(stderr, "bad --xfile\n");
            free(x);
            contract_free(&c);
            btn_free(&btn);
            cnb_free(&base);
            hybrid_ai_free(&cov);
            return 1;
        }
        if ((size_t)nx != btn.input_count) {
            fprintf(stderr, "x dim %d != unit in %zu\n", nx, btn.input_count);
            free(x);
            contract_free(&c);
            btn_free(&btn);
            cnb_free(&base);
            hybrid_ai_free(&cov);
            return 1;
        }
        y = btn_forward(&btn, x);
        if (!y) {
            fprintf(stderr, "btn_forward failed\n");
            free(x);
            contract_free(&c);
            btn_free(&btn);
            cnb_free(&base);
            hybrid_ai_free(&cov);
            return 1;
        }
        if (outpath) {
            FILE *f = fopen(outpath, "wb");
            if (!f) {
                free(x);
                contract_free(&c);
                btn_free(&btn);
                cnb_free(&base);
                hybrid_ai_free(&cov);
                return 1;
            }
            for (i = 0; i < (int)btn.output_count; i++)
                fprintf(f, "%s%.17g", i ? "," : "", y[i]);
            fprintf(f, "\n");
            fclose(f);
        } else {
            for (i = 0; i < (int)btn.output_count; i++)
                printf("%s%.17g", i ? "," : "", y[i]);
            printf("\n");
        }
        free(x);
    }

    contract_free(&c);
    btn_free(&btn);
    cnb_free(&base);
    hybrid_ai_free(&cov);
    return 0;
}
