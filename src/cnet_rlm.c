#include "cnet_rlm.h"

#include "cnet_skill_lane.h"

#include <stdio.h>
#include <string.h>

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t n;
    if (dst == NULL || cap == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void cnet_rlm_policy_default(CnetRlmPolicy *p) {
    if (p == NULL) return;
    memset(p, 0, sizeof *p);
    cnet_hemi_policy_default(&p->core);
    p->max_steps = 3;
    p->use_capsule_loop = 1;
}

static void clear_rlm(CnetRlmResult *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
}

static void hemi_from_capsule(const CnetCapsuleLoopResult *loop,
                              CnetHemiResult *h) {
    memset(h, 0, sizeof *h);
    h->via_core = 1;
    h->intent = CNET_CORE_INTENT_LOGIC;
    if (loop == NULL) {
        h->source = CNET_HEMI_SRC_ABSTAIN;
        return;
    }
    copy_text(h->skill, sizeof h->skill, loop->skill);
    copy_text(h->value, sizeof h->value, loop->value);
    copy_text(h->spoken, sizeof h->spoken, loop->spoken);
    copy_text(h->refusal, sizeof h->refusal, loop->refusal);
    h->bound = loop->bound;
    h->claimed_cert = loop->claimed_cert;
    h->residual_calls = 0;
    h->teacher_calls = 0;
    h->open_chat = 0;
    if (loop->bound && loop->claimed_cert) {
        h->hemi = CNET_HEMI_CORE;
        h->plane = CNET_CORE_PLANE_CERT;
        h->source = CNET_HEMI_SRC_SKILL_EXACT;
        h->may_voice = 1;
    } else {
        h->hemi = CNET_HEMI_NONE;
        h->plane = CNET_CORE_PLANE_NONE;
        h->source = CNET_HEMI_SRC_ABSTAIN;
        h->may_voice = 0;
        if (h->refusal[0] == '\0')
            copy_text(h->refusal, sizeof h->refusal, "capsule_abstain");
    }
}

static int push_step(CnetRlmResult *out, const CnetHemiResult *h,
                     const char *note) {
    CnetRlmStep *s;
    if (out == NULL || out->n_steps >= CNET_RLM_MAX_STEPS) return -1;
    s = &out->steps[out->n_steps++];
    memset(s, 0, sizeof *s);
    if (h) s->step = *h;
    copy_text(s->note, sizeof s->note, note ? note : "step");
    return 0;
}

static void set_final(CnetRlmResult *out, const CnetHemiResult *h) {
    if (out == NULL || h == NULL) return;
    out->final = *h;
    out->final.via_core = 1;
    if (h->spoken[0])
        copy_text(out->summary, sizeof out->summary, h->spoken);
    else if (h->value[0])
        copy_text(out->summary, sizeof out->summary, h->value);
    else if (h->refusal[0])
        copy_text(out->summary, sizeof out->summary, h->refusal);
}

int cnet_rlm_ask(const char *turn, const CnetRlmPolicy *policy,
                 CnetRlmResult *out) {
    CnetRlmPolicy pol;
    CnetHemiResult hr;
    int steps_left;
    int n_subj;

    if (out == NULL) return -1;
    clear_rlm(out);
    out->via_rlm = 1;
    if (policy)
        pol = *policy;
    else
        cnet_rlm_policy_default(&pol);
    if (pol.max_steps < 1) pol.max_steps = 1;
    if (pol.max_steps > CNET_RLM_MAX_STEPS) pol.max_steps = CNET_RLM_MAX_STEPS;

    if (turn == NULL || turn[0] == '\0') {
        out->intent = CNET_CORE_INTENT_UNKNOWN;
        out->final.via_core = 1;
        out->final.source = CNET_HEMI_SRC_ABSTAIN;
        copy_text(out->final.refusal, sizeof out->final.refusal, "empty_turn");
        copy_text(out->summary, sizeof out->summary, "empty_turn");
        push_step(out, &out->final, "empty");
        return 1;
    }

    out->intent = cnet_core_discern(turn);
    steps_left = pol.max_steps;

    /* Multi-skill CERT path: capsule_loop is CORE's bounded recursive CERT log */
    n_subj = cnet_capsule_loop_count_subjects(turn);
    if (pol.use_capsule_loop && n_subj >= 2 && steps_left > 0) {
        CnetCapsuleLoopResult loop;
        memset(&loop, 0, sizeof loop);
        if (cnet_capsule_loop_cd_ask(turn, NULL, &loop) == 0) {
            hemi_from_capsule(&loop, &hr);
            hr.intent = out->intent;
            push_step(out, &hr, "capsule");
            steps_left--;
            if (hr.bound && hr.plane == CNET_CORE_PLANE_CERT) {
                set_final(out, &hr);
                return 0;
            }
            /* capsule abstain → fall through to single core_ask once */
        }
    }

    /* Recursive CORE re-entry budget (usually 1 decisive core_ask). */
    while (steps_left > 0) {
        memset(&hr, 0, sizeof hr);
        if (cnet_core_ask(turn, &pol.core, &hr) < 0) return -1;
        hr.intent = out->intent;
        push_step(out, &hr, "core");
        steps_left--;

        if (hr.bound && hr.plane == CNET_CORE_PLANE_CERT) {
            set_final(out, &hr);
            return 0;
        }
        if (hr.bound && hr.plane == CNET_CORE_PLANE_OPEN_CHAT) {
            /* Creativity plane draft — RLM accepts as final (never CERT). */
            set_final(out, &hr);
            return 0;
        }

        /* Abstain: no further magic recursion into LLM for pure logic */
        if (out->intent == CNET_CORE_INTENT_LOGIC &&
            !pol.core.logic_open_chat_fallback) {
            set_final(out, &hr);
            return 1;
        }

        /* One more try only if open chat was disabled mid-flight — stop */
        set_final(out, &hr);
        return 1;
    }

    out->final.via_core = 1;
    out->final.source = CNET_HEMI_SRC_ABSTAIN;
    copy_text(out->final.refusal, sizeof out->final.refusal, "rlm_budget");
    copy_text(out->summary, sizeof out->summary, "rlm_budget");
    push_step(out, &out->final, "budget");
    return 1;
}
