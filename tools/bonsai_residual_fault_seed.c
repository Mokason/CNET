/* Seed fault-bus labeled vectors from Bonsai HTTP residual window steps.
 * Usage: CNET_RESIDUAL_HTTP=... CNET_FAULT_LOG=... ./bin/bonsai_residual_fault_seed [n]
 * make bonsai_residual_fault_seed
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/residual_http.h"
#include "../include/cnet_fault.h"
#include "../include/cnet_acct.h"

int main(int argc, char **argv) {
    const char *url = getenv("CNET_RESIDUAL_HTTP");
    const char *win = getenv("CNET_RESIDUAL_WINDOW");
    const char *flog = getenv("CNET_FAULT_LOG");
    ResidualHttp *r = NULL;
    int n = 32, i, ok = 0, fail = 0;
    double *in = NULL, *out = NULL;
    size_t W;

    if (argc >= 2) n = atoi(argv[1]);
    if (n < 1) n = 1;
    if (!url || !url[0]) {
        fprintf(stderr, "set CNET_RESIDUAL_HTTP\n");
        return 2;
    }
    if (!flog || !flog[0]) {
        fprintf(stderr, "set CNET_FAULT_LOG\n");
        return 2;
    }
    if (residual_http_open(&r, url, win, 32) != 0 || !r) {
        fprintf(stderr, "residual_http_open failed\n");
        return 3;
    }
    if (residual_http_ping(r) != 0) {
        fprintf(stderr, "ping failed\n");
        residual_http_close(r);
        return 4;
    }
    W = (size_t)residual_http_window_n(r);
    in = calloc(W, sizeof(double));
    out = calloc(W, sizeof(double));
    if (!in || !out) {
        residual_http_close(r);
        return 5;
    }
    if (n > (int)W) n = (int)W;
    for (i = 0; i < n; i++) {
        size_t j;
        for (j = 0; j < W; j++) in[j] = 0.0;
        in[i] = 1.0;
        if (residual_http_oracle(in, out, r) != 0) {
            fail++;
            continue;
        }
        /* Labeled residual correction pair for PEFT bus. */
        cnet_fault_mirror_labeled("bonsai_res_win", in, out, W, W,
                                  "residual_http");
        ok++;
        if (getenv("CNET_ACCT_LOG")) cnet_acct_add_tier_c();
    }
    printf("BONSAI_FAULT_SEED ok=%d fail=%d window=%zu fault_log=%s\n", ok,
           fail, W, flog);
    free(in);
    free(out);
    residual_http_close(r);
    return fail && !ok ? 1 : 0;
}
