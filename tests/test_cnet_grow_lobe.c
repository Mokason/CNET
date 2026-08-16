/* grow_lobe_v1 — parallel train lobe
 * External labels only. Refuse Core Tier-A. QAT student speaks after train.
 */
#include "../include/cnet_grow_lobe.h"
#include "../include/cnet_held_model.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int teacher_hook(const char *turn, char *out, size_t cap) {
    (void)turn;
    if (out == NULL || cap == 0) return 1;
    snprintf(out, cap,
             "When the river froze, the village stored grain for spring.");
    return 0;
}

static int english_chars(const char *s) {
    int n = 0, i;
    if (s == NULL || s[0] == '\0') return 0;
    for (i = 0; s[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalpha(c) || c == ' ' || c == '.' || c == ',' || c == '\''))
            return 0;
        if (isalpha(c)) n++;
    }
    return n > 0;
}

static int failures;
static int checks;

static void check(int cond, const char *msg) {
    checks++;
    if (cond)
        printf("  ok   %s\n", msg);
    else {
        printf("  FAIL %s\n", msg);
        failures++;
    }
}

int main(void) {
    CnetGrowLobe *l = NULL;
    CnetGrowPair ext, bad, wrap;
    double loss0 = 0, loss1 = 0;

    printf("cnet_grow_lobe — PARALLEL TRAIN LOBE / TERNARY QAT\n");

    memset(&ext, 0, sizeof ext);
    snprintf(ext.text, sizeof ext.text, "teacher: next is one");
    ext.target = 1;
    ext.source = CNET_GROW_SRC_EXTERNAL;
    check(cnet_grow_admit(&ext) == 0, "admit_external");

    memset(&bad, 0, sizeof bad);
    snprintf(bad.text, sizeof bad.text, "apply increment_mod256 to 41");
    bad.target = 42;
    bad.source = CNET_GROW_SRC_TIER_A;
    check(cnet_grow_admit(&bad) == 1, "refuse_tier_a_source");

    memset(&wrap, 0, sizeof wrap);
    snprintf(wrap.text, sizeof wrap.text, "Marble reports 42.");
    wrap.target = 2;
    wrap.source = CNET_GROW_SRC_EXTERNAL;
    check(cnet_grow_admit(&wrap) == 1, "refuse_tier_a_text");

    check(cnet_grow_open(&l) == 0 && l != NULL, "open");
    check(cnet_grow_may_speak(l) == 0, "may_speak_before_train");
    check(cnet_grow_claimed_cert(l) == 0, "no_cert");

    check(cnet_grow_ingest(l, &bad) == 1, "ingest_tier_a_rc");
    check(cnet_grow_refused(l) == 1, "refused_1");
    check(cnet_grow_count(l) == 0, "count_empty_after_refuse");

    {
        const char *asks[] = {"Write one calm English sentence about winter."};
        cnet_held_model_set_hook(teacher_hook);
        check(cnet_grow_distill_held(l, asks, 1) == 1, "distill_held_english");
        cnet_held_model_set_hook(NULL);
    }
    {
        char frame[CNET_GROW_TEXT];
        check(cnet_grow_wrap_chat(frame, sizeof frame, "hi", "hello.") == 0,
              "wrap_chat_rc");
        check(strstr(frame, "<|im_start|>system") != NULL, "wrap_system");
        check(strstr(frame, "<|im_start|>user") != NULL, "wrap_user");
        check(strstr(frame, "<|im_start|>assistant") != NULL, "wrap_asst");
        check(strstr(frame, "<|im_end|>") != NULL, "wrap_im_end");
    }
    check(cnet_grow_seed_chat_template(l) >= 3, "seed_chat_template");
    check(cnet_grow_seed_english(l) >= 8, "seed_english");
    check(cnet_grow_count(l) >= 12, "count_english");
    check(cnet_grow_refused(l) == 1, "still_one_refuse");

    check(cnet_grow_train(l, 4, &loss0) == 0, "train_warmup");
    check(cnet_grow_train(l, 12, &loss1) == 0, "train_english");
    check(loss1 <= loss0 + 2.0, "loss_not_explode");
    check(cnet_grow_may_speak(l) == 1, "may_speak_after_train");
    {
        char spoken[48];
        CnetGrowLobe *cat = NULL;
        int k;
        check(cnet_grow_speak(l, "the cat sat on the ma", spoken, sizeof spoken,
                              8) == 0,
              "speak_rc");
        check(spoken[0] != '\0', "speak_nonempty");
        check(english_chars(spoken), "speak_english_charset");
        check(cnet_grow_open(&cat) == 0 && cat != NULL, "cat_open");
        for (k = 0; k < 8; ++k) {
            CnetGrowPair p;
            memset(&p, 0, sizeof p);
            snprintf(p.text, sizeof p.text, "the cat sat on the mat. ");
            p.source = CNET_GROW_SRC_EXTERNAL;
            cnet_grow_ingest(cat, &p);
        }
        check(cnet_grow_train(cat, 16, NULL) == 0, "cat_train");
        check(cnet_grow_speak(cat, "the cat sat on the ma", spoken, sizeof spoken,
                              4) == 0,
              "cat_speak_rc");
        if (spoken[0] != 't')
            printf("  note cat_spoken=[%s]\n", spoken);
        check(spoken[0] == 't', "speak_mat_t");
        check(cnet_grow_claimed_cert(l) == 0, "speak_no_cert");
        cnet_grow_close(cat);
    }
    {
        CnetGrowLobe *fit = NULL;
        double recent = 0.0;
        int k;
        check(cnet_grow_open(&fit) == 0 && fit != NULL, "fit_open");
        for (k = 0; k < 4; ++k) {
            CnetGrowPair p;
            memset(&p, 0, sizeof p);
            snprintf(p.text, sizeof p.text, "the sun is warm today. ");
            p.source = CNET_GROW_SRC_EXTERNAL;
            cnet_grow_ingest(fit, &p);
        }
        check(cnet_grow_train_last(fit, 20, NULL) == 0, "fit_train_last_fp");
        check(cnet_grow_train_recent(fit, 4, 2, NULL) == 0, "fit_train_recent");
        check(cnet_grow_prefix_acc_recent(fit, 4, &recent) == 0, "fit_acc_rc");
        if (recent < 0.40)
            printf("  note recent_acc=%.3f\n", recent);
        check(recent >= 0.40, "recent_acc_climbs");
        cnet_grow_close(fit);
    }

    cnet_grow_close(l);

    if (failures) {
        printf("CNET_GROW_RED failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("CNET_GROW_PASS\n");
    printf("checks=%d may_speak=1 claimed_cert=0 anti_collapse=1 "
           "ternary_qat=1 english=1 python=0 broader_claims=WITHHELD\n",
           checks);
    return 0;
}
