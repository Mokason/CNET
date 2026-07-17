/* E: residual session chat helper (opt-in session KV).
 * Build: make residual_session_chat
 * Usage: CNET_RESIDUAL_GGUF=... CNET_RESIDUAL_SESSION_KV=1 bin/residual_session_chat
 *   stdin: one hot index per line (0..W-1); prints argmax next window slot.
 * Demonstrates multi-turn residual without Hermes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/residual_gguf.h"

int main(void) {
    ResidualGguf *r = NULL;
    const char *path = getenv("CNET_RESIDUAL_GGUF");
    const char *win = getenv("CNET_RESIDUAL_WINDOW");
    double *in, *out;
    int n, i, hot, best;
    char line[64];

    if (!path || !path[0]) {
        fprintf(stderr, "set CNET_RESIDUAL_GGUF\n");
        return 2;
    }
    setenv("CNET_RESIDUAL_SESSION_KV", "1", 0);
    if (residual_gguf_open(&r, path, win, 32) != 0) {
        fprintf(stderr, "residual open failed\n");
        return 1;
    }
    n = residual_gguf_window_n(r);
    in = calloc((size_t)n, sizeof(double));
    out = calloc((size_t)n, sizeof(double));
    printf("residual_session_chat window=%d session=%d\n", n,
           residual_gguf_session_mode(r));
    printf("enter window index 0..%d (or q)\n", n - 1);
    while (fgets(line, sizeof line, stdin)) {
        if (line[0] == 'q' || line[0] == 'Q') break;
        if (line[0] == 'r' || line[0] == 'R') {
            residual_gguf_session_reset(r);
            printf("session reset\n");
            continue;
        }
        hot = atoi(line);
        if (hot < 0 || hot >= n) {
            printf("bad index\n");
            continue;
        }
        for (i = 0; i < n; i++) in[i] = 0.0;
        in[hot] = 1.0;
        if (residual_gguf_oracle(in, out, r) != 0) {
            printf("forward fail\n");
            continue;
        }
        best = 0;
        for (i = 1; i < n; i++)
            if (out[i] > out[best]) best = i;
        printf("in=%d -> next=%d (pilot_recorded=%llu)\n", hot, best,
               (unsigned long long)residual_gguf_pilot_recorded(r));
    }
    free(in);
    free(out);
    residual_gguf_close(r);
    return 0;
}
