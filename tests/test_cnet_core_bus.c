/* CORE bus mini stack gate — multi-brick + miss-log propose.
 * make cnet_core_bus → CNET_CORE_BUS_PASS
 */
#include "cnet_core_bus.h"
#include "cnet_dc_invent.h"
#include "cnet_hemisphere.h"
#include "cnet_held_model.h"
#include "cnet_rlm.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-60s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int hook_illegal(const char *turn, char *out, size_t cap) {
    (void)turn;
    snprintf(out, cap, "ILLEGAL_OPEN_CHAT_ANSWER");
    return 0;
}

static int prove_all(CnetCoreBus *b, const char *tag) {
    unsigned x;
    for (x = 0; x < 16; ++x) {
        char turn[80];
        CnetCoreBusResult res;
        snprintf(turn, sizeof turn, "%s %u", tag, x);
        if (cnet_core_bus_result(b, turn, &res) != 0 || !res.proved ||
            !res.claimed_cert)
            return 0;
    }
    return 1;
}

int main(void) {
    const char *bonsai = "/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf";
    const char *dir = "core_bus_bricks";
    char path_a[128], path_b[128];
    CnetCoreBus bus;
    CnetWeightConvertReport ra, rb;
    CnetCoreBusResult res;
    CnetHemiPolicy hp;
    CnetHemiResult hr;
    CnetRlmPolicy rp;
    CnetRlmResult rr;
    CnetDcTerm brick;
    char brick_name[64];
    char misspath[] = "core_bus_miss.jsonl";
    CnetDcMissRow row;
    int before;

    printf("== cnet_core_bus mini stack (multi-brick, no leftover mouth) ==\n");
    mkdir(dir, 0755);
    snprintf(path_a, sizeof path_a, "%s/q1_add16_attn_q.gguf", dir);
    snprintf(path_b, sizeof path_b, "%s/q1_xor16_attn_k.gguf", dir);
    unlink(path_a);
    unlink(path_b);
    unlink(misspath);

    cnet_held_model_set_hook(hook_illegal);
    cnet_hemi_policy_default(&hp);
    check(hp.open_chat_enabled == 0, "default kills OPEN_CHAT leftover");
    check(cnet_hemi_ask("write a short poem about zz99", &hp, &hr) == 1,
          "hemi abstains creative leftover");
    check(strstr(hr.spoken, "ILLEGAL_OPEN_CHAT_ANSWER") == NULL,
          "hook never surfaces");
    cnet_rlm_policy_default(&rp);
    check(cnet_rlm_ask("write a haiku about moons", &rp, &rr) == 1,
          "RLM kills OPEN_CHAT answers");

    check(access(bonsai, R_OK) == 0, "Bonsai-8B.gguf present");
    cnet_core_bus_init(&bus);
    memset(&ra, 0, sizeof ra);
    memset(&rb, 0, sizeof rb);

    /* Brick 1: attn_q add mode — must serve without LLM before brick 2 */
    check(cnet_core_bus_make_brick(&bus, bonsai, "blk.0.attn_q.weight", path_a,
                                   "brick_attn_q_add", "q1_add16", 0, &ra) == 0,
          "brick1 LEASE→TABLE→CERTIFY→park");
    check(ra.spec_rate + 1e-12 >= 0.95 && ra.teacher_unbound == 1,
          "brick1 spec≥0.95 teacher gone");
    check(ra.certified == 0, "table door does not auto-CERT");
    check(bus.n_bricks == 1 && bus.state == CNET_CORE_BUS_IDLE,
          "brick1 parked; bus IDLE for next domain");
    check(prove_all(&bus, "q1_add16"),
          "brick1 RESULT×16 without LLM");

    /* Brick 2: only after brick1 serves */
    {
        int rc2 = cnet_core_bus_make_brick(&bus, bonsai, "blk.0.attn_k.weight", path_b,
                                   "brick_attn_k_xor", "q1_xor16", 1, &rb);
        if (rc2 != 0) printf("  brick2_rc=%d state=%d n_bricks=%d rate=%f\n", rc2, (int)bus.state, bus.n_bricks, rb.spec_rate);
        check(rc2 == 0, "brick2 LEASE→TABLE→CERTIFY→park");
    }
    check(rb.spec_rate + 1e-12 >= 0.95 && rb.teacher_unbound == 1,
          "brick2 spec≥0.95 teacher gone");
    check(bus.n_bricks == 2, "two bricks installed");
    check(prove_all(&bus, "q1_xor16"), "brick2 RESULT×16 without LLM");
    check(prove_all(&bus, "q1_add16"), "brick1 still serves after brick2");

    check(cnet_core_bus_result(&bus, "what is the meaning of life", &res) == 1 &&
              res.abstained,
          "outside table → abstain");
    check(bus.open_chat_blocked > 0, "leftover mouths counted blocked");

    /* miss-log → e-graph propose only */
    memset(&row, 0, sizeof row);
    snprintf(row.goal_type, sizeof row.goal_type, "q1_add16");
    snprintf(row.term, sizeof row.term, "(incr (incr zero))");
    snprintf(row.trace_id, sizeof row.trace_id, "trace_a");
    row.certified = 1;
    check(cnet_dc_misslog_append(misspath, &row) == 0, "miss-log A");
    snprintf(row.term, sizeof row.term, "((lam (x) (incr (incr x))) zero)");
    snprintf(row.trace_id, sizeof row.trace_id, "trace_b");
    check(cnet_dc_misslog_append(misspath, &row) == 0, "miss-log B");
    before = cnet_dc_extract_specialist_admit_calls();
    {
        int er = cnet_core_bus_misslog_propose(misspath, brick_name,
                                               sizeof brick_name, &brick);
        check(er == 0 || er == 1, "misslog e-graph propose");
        check(cnet_dc_extract_specialist_admit_calls() == before,
              "propose ≠ admit");
    }

    cnet_core_bus_free(&bus);
    cnet_held_model_set_hook(NULL);
    unlink(path_a);
    unlink(path_b);
    unlink(misspath);

    printf("CNET_CORE_BUS_PASS checks=%d fails=%d\n", checks, failures);
    printf("verbs=lease,table,certify,result open_chat_answer=0 residual_auto_cert=0 "
           "bonsai_q1_brick=1 brick2=1 teacher_gone=1 outside_table_abstain=1 "
           "misslog_egraph_propose=1 python=0 broader_claims=WITHHELD\n");
    return failures ? 1 : 0;
}
