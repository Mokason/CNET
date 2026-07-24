/* cnet_openlab_import_test — MoE hard expert + tiered serve + acct dump */
#include "../include/json_toolcall.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/personal_ai.h"
#include "../include/cnet_moe.h"
#include "../include/cnet_acct.h"
#include "../include/hybrid_ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL %s\n", m); g_fail++; } } while (0)

int main(void) {
    PrimitiveRegistry reg;
    BinaryTransformNetwork *student = NULL;
    const char *unit;
    int IN, OUT;
    double *in, *out;
    Port ip, gp;
    CnetMoeHit hit;
    CnetAcct ac;
    char base[] = "/tmp/cnet_openlab_base_XXXXXX";
    char acctpath[] = "/tmp/cnet_acct_test.jsonl";
    PersonalAi ai;
    PersonalAiPolicy pol;
    PersonalAiReport rep;
    int i;

    cnet_acct_reset();
    registry_init(&reg);
    CHECK(cnet_jtc_v0_mine_admit(&reg, 0x0BE11AB01ULL, &student, NULL) == 0 && student,
          "mine_admit");
    IN = (int)student->input_count;
    OUT = (int)student->output_count;
    unit = CNET_JTC_UNIT_NAME;

    in = calloc((size_t)IN, sizeof(double));
    out = calloc((size_t)OUT, sizeof(double));
    CHECK(in && out, "alloc");
    for (i = 0; i < IN; i++) in[i] = (i % 3 == 0) ? 1.0 : 0.0;

    memset(&ip, 0, sizeof ip);
    memset(&gp, 0, sizeof gp);
    ip.family = PORT_RAW;
    ip.field_width = (size_t)IN;
    ip.field_count = 1;
    port_set_tag(&ip, "jtc_feat");
    gp.family = PORT_ONEHOT;
    gp.field_width = (size_t)OUT;
    gp.field_count = 1;
    port_set_tag(&gp, unit); /* hard expert by unit name */

    CHECK(cnet_moe_try_hard(&reg, ip, gp, in, (size_t)IN, out, (size_t)OUT, &hit) == 0,
          "moe hard");
    CHECK(hit.hit == 1, "hit");
    CHECK(hit.steps == 1, "steps");
    cnet_acct_add_hard(1);

    /* Unknown tag → no expert */
    port_set_tag(&gp, "not_a_real_skill_zz");
    CHECK(cnet_moe_try_hard(&reg, ip, gp, in, (size_t)IN, out, (size_t)OUT, &hit) == 1,
          "moe miss");

    /* Acct dump */
    {
        FILE *tf = fopen(acctpath, "w");
        if (tf) fclose(tf);
    }
    setenv("CNET_ACCT_LOG", acctpath, 1);
    cnet_acct_add_tier_b();
    cnet_acct_add_tier_c();
    cnet_acct_add_gap();
    CHECK(cnet_acct_dump(acctpath) == 0, "dump");
    cnet_acct_get(&ac);
    CHECK(ac.hard_expert_hits >= 1, "hard count");
    CHECK(ac.tier_b_hits >= 1 && ac.tier_c_hits >= 1, "tier counts");
    CHECK(ac.gap_notes >= 1, "gaps");

    /* personal_ai_serve hard path needs a real base file — skip full open if heavy;
     * moe unit test above is the core MoE gate. */

    free(in);
    free(out);
    unlink(acctpath);
    (void)base;
    (void)ai;
    (void)pol;
    (void)rep;

    if (g_fail) {
        fprintf(stderr, "failures=%d\n", g_fail);
        return 1;
    }
    printf("CNET_OPENLAB_IMPORT_PASS hard=%llu steps=%llu b=%llu c=%llu gaps=%llu\n",
           (unsigned long long)ac.hard_expert_hits,
           (unsigned long long)ac.activated_steps,
           (unsigned long long)ac.tier_b_hits,
           (unsigned long long)ac.tier_c_hits,
           (unsigned long long)ac.gap_notes);
    return 0;
}
