/* Residual GGUF gate: hermetic always; real load when CNET_RESIDUAL_GGUF set.
 * make residual_gguf → RESIDUAL_GGUF_PASS
 * make residual_gguf_real → requires model path (+ optional structure mine)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/residual_gguf.h"
#include "../include/personal_ai.h"
#include "../include/hybrid_ai.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    const char *path = getenv("CNET_RESIDUAL_GGUF");
    const char *win = getenv("CNET_RESIDUAL_WINDOW");
    int require_real = getenv("CNET_REQUIRE_REAL_RESIDUAL") &&
                       getenv("CNET_REQUIRE_REAL_RESIDUAL")[0] == '1';
    int do_mine = getenv("CNET_RESIDUAL_STRUCTURE_MINE") &&
                  getenv("CNET_RESIDUAL_STRUCTURE_MINE")[0] == '1';

    printf("== residual GGUF (Tier C real-world) ==\n");

    check(personal_ai_auto_residual_gguf(NULL, NULL) < 0,
          "auto-bind refuses NULL ai");

    {
        PersonalAi ai;
        PersonalAiPolicy pol;
        ResidualGguf *owned = NULL;
        int open_rc;
        personal_ai_policy_defaults(&pol);
        remove("tmp_res_gguf.cnb");
        remove("tmp_res_gguf.gaps.txt");
        open_rc = personal_ai_open(&ai, "tmp_res_gguf.cnb", "tmp_res_gguf.gaps.txt",
                                   NULL, &pol);
        if (!path || !path[0]) {
            check(open_rc == 0, "personal_ai opens");
            if (open_rc == 0) {
                int ar = personal_ai_auto_residual_gguf(&ai, &owned);
                check(ar == 1 && owned == NULL,
                      "auto-bind skips when CNET_RESIDUAL_GGUF unset");
                personal_ai_close(&ai);
            }
        } else if (open_rc != 0) {
            check(!require_real,
                  require_real
                      ? "REQUIRED real residual open failed"
                      : "optional real residual open skipped");
            if (require_real)
                printf("  (path=%s win=%s open_rc=%d)\n", path,
                       win ? win : "(synth)", open_rc);
        } else {
            check(1, "personal_ai opens with residual env");
            check(ai.owned_residual != NULL && ai.hybrid.residual.bound,
                  "open auto-bound real residual");
            {
                int ar = personal_ai_auto_residual_gguf(&ai, &owned);
                check(ar == 0 && owned == ai.owned_residual,
                      "auto-bind reuses open residual");
            }
            owned = ai.owned_residual;
            check(residual_gguf_window_n(owned) > 0, "real residual has window");
            check(residual_gguf_vocab(owned) > 0, "real residual has vocab");
            {
                Port pin = residual_gguf_input_port(owned);
                Port pout = residual_gguf_output_port(owned);
                double *in, *out;
                size_t n = (size_t)residual_gguf_window_n(owned);
                size_t i;
                int rc;
                PersonalAiReport rep;
                in = calloc(n, sizeof(double));
                out = calloc(n, sizeof(double));
                check(in && out, "buffers");
                in[0] = 1.0;
                rc = residual_gguf_oracle(in, out, owned);
                check(rc == 0, "oracle forward succeeds");
                {
                    int hot = 0;
                    for (i = 1; i < n; i++)
                        if (out[i] > out[hot]) hot = (int)i;
                    check(out[hot] == 1.0, "output is one-hot in window");
                }
                /* Full cascade: no A/B for these ports → residual C */
                rc = personal_ai_serve(&ai, pin, pout, in, n, out, n, &rep);
                check(rc == 0 && rep.source == PERSONAL_AI_RESIDUAL &&
                          rep.trust == HYBRID_TRUST_UNCERTIFIED,
                      "personal_ai serves real residual as Tier C");

                /* P5: hit residual enough times, mine structure → Tier A */
                if (do_mine || require_real) {
                    size_t reg_before = ai.lane.reg.count;
                    BinaryTransformNetwork *stu = NULL;
                    int mrc;
                    for (i = 0; i < 3; i++) {
                        size_t j;
                        for (j = 0; j < n; j++) in[j] = 0.0;
                        in[i % (n < 4 ? n : 4)] = 1.0;
                        personal_ai_serve(&ai, pin, pout, in, n, out, n, &rep);
                    }
                    check(ai.hybrid.trace_count >= 1 &&
                              ai.hybrid.traces[0].hits >= 3,
                          "residual traces accumulate for mining");
                    mrc = personal_ai_structure_mine(&ai, &stu);
                    check(mrc == 0 && stu != NULL &&
                              ai.lane.reg.count > reg_before,
                          "P5 structure mine admits unit from residual");
                    if (mrc == 0 && stu) {
                        /* Serve again — prefer local A if route finds the unit */
                        for (i = 0; i < n; i++) in[i] = 0.0;
                        in[0] = 1.0;
                        rc = personal_ai_serve(&ai, pin, pout, in, n, out, n,
                                               &rep);
                        check(rc == 0 &&
                                  (rep.source == PERSONAL_AI_LOCAL ||
                                   rep.source == PERSONAL_AI_RESIDUAL),
                              "after mine, serve local A or residual C");
                        if (rep.source == PERSONAL_AI_LOCAL)
                            printf("  (post-mine source=local Tier A)\n");
                        else
                            printf("  (post-mine source=residual; unit admitted "
                                   "reg=%zu)\n",
                                   ai.lane.reg.count);
                    }
                }
                free(in);
                free(out);
            }
            /* owned residual freed by personal_ai_close — do not double-free */
            personal_ai_close(&ai);
        }
        remove("tmp_res_gguf.cnb");
        remove("tmp_res_gguf.gaps.txt");
    }

    /* Bad path refuses cleanly */
    {
        ResidualGguf *r = NULL;
        check(residual_gguf_open(&r, "/nonexistent/model.gguf", NULL, 16) != 0 &&
                  r == NULL,
              "missing GGUF refuses cleanly");
    }

    /* Fail-closed open when residual env points at missing file */
    {
        const char *old = path;
        PersonalAi ai;
        PersonalAiPolicy pol;
        personal_ai_policy_defaults(&pol);
        setenv("CNET_RESIDUAL_GGUF", "/nonexistent/nope.gguf", 1);
        remove("tmp_res_fail.cnb");
        remove("tmp_res_fail.gaps.txt");
        check(personal_ai_open(&ai, "tmp_res_fail.cnb", "tmp_res_fail.gaps.txt",
                               NULL, &pol) == -4,
              "open fails closed on missing residual GGUF");
        if (old && old[0])
            setenv("CNET_RESIDUAL_GGUF", old, 1);
        else
            unsetenv("CNET_RESIDUAL_GGUF");
        remove("tmp_res_fail.cnb");
        remove("tmp_res_fail.gaps.txt");
    }

    if (failures) {
        printf("RESIDUAL_GGUF_FAIL failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("RESIDUAL_GGUF_PASS checks=%d\n", checks);
    return 0;
}
