/* E: residual session chat helper (opt-in session KV + MTK skills).
 * Build: make residual_session_chat
 * Usage:
 *   CNET_RESIDUAL_GGUF=... CNET_RESIDUAL_SESSION_KV=1 \
 *   CNET_MTK=1 CNET_MTK_ROUTES=config/mtk_routes.example \
 *   bin/residual_session_chat
 * stdin:
 *   <index>           one-hot window slot 0..W-1
 *   p <text>          MTK route prompt (skill cartridge)
 *   a <skill.cmsk>    apply skill file
 *   u                 revert skill
 *   r                 session KV reset
 *   q                 quit
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
    char line[512];

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
    printf("residual_session_chat window=%d session=%d mtk=%s\n", n,
           residual_gguf_session_mode(r),
           residual_gguf_mtk(r) ? "on" : "off");
    printf("commands: <idx> | p <prompt> | a <skill> | u | r | q\n");
    while (fgets(line, sizeof line, stdin)) {
        if (line[0] == 'q' || line[0] == 'Q') break;
        if (line[0] == 'r' || line[0] == 'R') {
            if (line[1] == 0 || line[1] == '\n' || line[1] == ' ') {
                residual_gguf_session_reset(r);
                printf("session reset\n");
                continue;
            }
        }
        if ((line[0] == 'p' || line[0] == 'P') &&
            (line[1] == ' ' || line[1] == '\t')) {
            int rc = residual_gguf_mtk_route(r, line + 2);
            printf("mtk route %s (active=%d)\n",
                   rc == 0 ? "ok" : (rc == 1 ? "no-match" : "err"),
                   residual_gguf_mtk_active(r));
            continue;
        }
        if ((line[0] == 'a' || line[0] == 'A') &&
            (line[1] == ' ' || line[1] == '\t')) {
            char *sp = line + 2;
            size_t L;
            while (*sp == ' ') sp++;
            L = strlen(sp);
            while (L > 0 && (sp[L - 1] == '\n' || sp[L - 1] == '\r'))
                sp[--L] = 0;
            printf("mtk apply %s\n",
                   residual_gguf_mtk_apply(r, sp) == 0 ? "ok" : "fail");
            continue;
        }
        if (line[0] == 'u' || line[0] == 'U') {
            printf("mtk revert %s\n",
                   residual_gguf_mtk_revert(r) == 0 ? "ok" : "fail");
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
        printf("in=%d -> next=%d mtk_active=%d (pilot_recorded=%llu)\n", hot,
               best, residual_gguf_mtk_active(r),
               (unsigned long long)residual_gguf_pilot_recorded(r));
    }
    free(in);
    free(out);
    residual_gguf_close(r);
    return 0;
}
