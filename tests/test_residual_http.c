/* HTTP residual gate: hermetic without URL; real when CNET_RESIDUAL_HTTP set.
 * make residual_http → RESIDUAL_HTTP_PASS
 * make residual_http_real → requires live llama-server (Bonsai)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/residual_http.h"
#include "../include/personal_ai.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    const char *url = getenv("CNET_RESIDUAL_HTTP");
    (void)getenv("CNET_RESIDUAL_WINDOW");
    int require = getenv("CNET_REQUIRE_REAL_RESIDUAL_HTTP") &&
                  getenv("CNET_REQUIRE_REAL_RESIDUAL_HTTP")[0] == '1';

    printf("== residual HTTP (Tier C Bonsai path) ==\n");

    check(personal_ai_auto_residual_http(NULL, NULL) < 0,
          "auto-bind refuses NULL ai");

    {
        PersonalAi ai;
        PersonalAiPolicy pol;
        ResidualHttp *owned = NULL;
        int open_rc;
        personal_ai_policy_defaults(&pol);
        remove("tmp_res_http.cnb");
        remove("tmp_res_http.gaps.txt");
        unsetenv("CNET_RESIDUAL_GGUF");
        if (!url || !url[0]) {
            open_rc = personal_ai_open(&ai, "tmp_res_http.cnb",
                                       "tmp_res_http.gaps.txt", NULL, &pol);
            check(open_rc == 0, "personal_ai opens without HTTP residual");
            if (open_rc == 0) {
                int ar = personal_ai_auto_residual_http(&ai, &owned);
                check(ar == 1 && owned == NULL,
                      "auto-bind skips when CNET_RESIDUAL_HTTP unset");
                personal_ai_close(&ai);
            }
            if (require) {
                check(0, "REQUIRED real HTTP residual but URL unset");
            }
        } else {
            open_rc = personal_ai_open(&ai, "tmp_res_http.cnb",
                                       "tmp_res_http.gaps.txt", NULL, &pol);
            if (open_rc != 0) {
                check(!require, "optional HTTP residual open skipped");
                if (require)
                    printf("  (url=%s open_rc=%d)\n", url, open_rc);
            } else {
                check(1, "personal_ai opens with HTTP residual env");
                check(ai.owned_residual_http != NULL && ai.hybrid.residual.bound,
                      "open auto-bound http residual");
                owned = ai.owned_residual_http;
                check(residual_http_window_n(owned) > 0, "http residual has window");
                check(residual_http_ping(owned) == 0, "http residual ping /v1/models");
                {
                    Port pin = residual_http_input_port(owned);
                    Port pout = residual_http_output_port(owned);
                    size_t n = (size_t)residual_http_window_n(owned);
                    double *in = calloc(n, sizeof(double));
                    double *out = calloc(n, sizeof(double));
                    PersonalAiReport rep;
                    int rc, i;
                    check(in && out, "alloc io");
                    if (in && out) {
                        in[0] = 1.0;
                        rc = residual_http_oracle(in, out, owned);
                        check(rc == 0, "http residual oracle one-step");
                        {
                            int hot = 0;
                            for (i = 1; i < (int)n; i++)
                                if (out[i] > out[hot]) hot = i;
                            check(out[hot] == 1.0, "oracle one-hot output");
                            printf("  oracle in_slot=0 -> out_slot=%d\n", hot);
                        }
                        memset(&rep, 0, sizeof rep);
                        rc = personal_ai_serve(&ai, pin, pout, in, n, out, n,
                                               &rep);
                        check(rc == 0, "personal_ai serves via http residual");
                        printf("  serve source=%d residual_hits=%zu\n",
                               (int)rep.source, rep.residual_hits);
                    }
                    free(in);
                    free(out);
                }
                personal_ai_close(&ai);
            }
        }
    }

    remove("tmp_res_http.cnb");
    remove("tmp_res_http.gaps.txt");
    remove("tmp_res_http.cnb.tmp");

    if (failures) {
        printf("RESIDUAL_HTTP_FAIL failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("RESIDUAL_HTTP_PASS checks=%d\n", checks);
    return 0;
}
