#include "cnet_capsule_loop.h"

#include "cnet_c_speak.h"
#include "cnet_lookup.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Bounded call log beside cnet_skill_lane. Reuses the hard table and
   bind path. Does not fork a second exact-lane law. Does not grow WordLM.
   Residual / teacher symbols are not invoked on this lane. */

static void copy_text(char *dst, size_t cap, const char *src)
{
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

static void clear_result(CnetCapsuleLoopResult *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
}

static CnetCapsulePerm tighten(CnetCapsulePerm prior, CnetCapsulePerm next)
{
    /* Monotonic: a later listener cannot undo a denial or an abstain. */
    if (prior == CNET_CAPSULE_PERM_DENY || next == CNET_CAPSULE_PERM_DENY)
        return CNET_CAPSULE_PERM_DENY;
    if (prior == CNET_CAPSULE_PERM_ABSTAIN || next == CNET_CAPSULE_PERM_ABSTAIN)
        return CNET_CAPSULE_PERM_ABSTAIN;
    return CNET_CAPSULE_PERM_ALLOW;
}

static int name_in_table(const char *name)
{
    char routed[CNET_SKILL_LANE_NAME];
    if (name == NULL || name[0] == '\0') return 0;
    routed[0] = '\0';
    if (cnet_skill_lane_route(name, routed, sizeof routed) != 0)
        return 0;
    return routed[0] != '\0';
}

CnetCapsulePerm cnet_capsule_loop_pre_execute(CnetCapsulePerm prior,
                                              const char *name,
                                              const CnetChatLookupTurn *hop,
                                              int verified_fixture)
{
    CnetCapsulePerm got = CNET_CAPSULE_PERM_ALLOW;

    if (hop != NULL && hop->residual_calls != 0)
        got = CNET_CAPSULE_PERM_DENY;
    else if (!verified_fixture)
        got = CNET_CAPSULE_PERM_ABSTAIN;
    else if (name == NULL || name[0] == '\0')
        got = CNET_CAPSULE_PERM_ABSTAIN;
    else if (!name_in_table(name))
        got = CNET_CAPSULE_PERM_DENY; /* unknown name, not nearest-skill */

    return tighten(prior, got);
}

/* Blank one whole word (same boundary rule as cnet_skill_lane_route). */
static void blank_word_ci(char *text, const char *word)
{
    char *cursor;
    size_t n, i;
    if (text == NULL || word == NULL || word[0] == '\0') return;
    n = strlen(word);
    for (cursor = text; *cursor != '\0'; ++cursor) {
        int left, right, match = 1;
        if (cursor > text && isalnum((unsigned char)cursor[-1])) continue;
        for (i = 0; i < n; ++i) {
            if (cursor[i] == '\0' ||
                tolower((unsigned char)cursor[i]) !=
                    tolower((unsigned char)word[i])) {
                match = 0;
                break;
            }
        }
        if (!match) continue;
        right = !isalnum((unsigned char)cursor[n]);
        left = (cursor == text) || !isalnum((unsigned char)cursor[-1]);
        if (left && right) {
            for (i = 0; i < n; ++i) cursor[i] = ' ';
            cursor += n - 1u;
        }
    }
}

/* Consume the routed skill so the next route() sees a different subject.
   Aliases match the skill-lane table; bind law stays in cnet_skill_lane. */
static void consume_skill(char *text, const char *canonical)
{
    if (text == NULL || canonical == NULL) return;
    blank_word_ci(text, canonical);
    if (strcmp(canonical, "increment_mod256") == 0)
        blank_word_ci(text, "increment");
    else if (strcmp(canonical, "crc8_atm") == 0)
        blank_word_ci(text, "crc8");
    else if (strcmp(canonical, CNET_LOOKUP_CONTRACT) == 0)
        blank_word_ci(text, "lookup");
}

int cnet_capsule_loop_count_subjects(const char *turn)
{
    char remaining[CNET_CAPSULE_LOOP_TEXT];
    char skill[CNET_SKILL_LANE_NAME];
    int n = 0;

    if (turn == NULL || turn[0] == '\0') return 0;
    copy_text(remaining, sizeof remaining, turn);
    while (n < 8) {
        skill[0] = '\0';
        if (cnet_skill_lane_route(remaining, skill, sizeof skill) != 0 ||
            skill[0] == '\0')
            break;
        n++;
        consume_skill(remaining, skill);
    }
    return n;
}

static void append_from_lane(CnetCapsuleLoopResult *out, const char *name,
                             const CnetSkillLaneResult *lane,
                             CnetCapsulePerm perm, CnetCapsuleCallKind kind)
{
    CnetCapsuleCall *c;
    if (out == NULL || out->n_calls >= CNET_CAPSULE_LOOP_MAX_STEPS) return;
    c = &out->calls[out->n_calls];
    memset(c, 0, sizeof *c);
    snprintf(c->call_id, sizeof c->call_id, "c%u", out->n_calls);
    copy_text(c->name, sizeof c->name,
              (name && name[0]) ? name : (lane ? lane->skill : ""));
    if (lane != NULL) {
        copy_text(c->value, sizeof c->value, lane->value);
        copy_text(c->refusal, sizeof c->refusal, lane->refusal);
        c->bound = lane->bound;
        c->claimed_cert = lane->claimed_cert;
    }
    c->perm = perm;
    c->kind = kind;
    c->residual_calls = 0;
    c->teacher_calls = 0;
    if (kind == CNET_CAPSULE_CALL_EXACT) {
        c->bound = 1;
        c->claimed_cert = 1;
        out->n_exact++;
    } else {
        c->bound = 0;
        c->claimed_cert = 0;
    }
    out->n_calls++;
}

static void append_refuse(CnetCapsuleLoopResult *out, const char *name,
                          const char *why, CnetCapsulePerm perm,
                          CnetCapsuleCallKind kind)
{
    CnetSkillLaneResult dummy;
    memset(&dummy, 0, sizeof dummy);
    copy_text(dummy.skill, sizeof dummy.skill, name ? name : "");
    copy_text(dummy.refusal, sizeof dummy.refusal, why);
    append_from_lane(out, name, &dummy, perm, kind);
}

static void speak_last(CnetCapsuleLoopResult *out)
{
    const CnetCapsuleCall *last_exact = NULL;
    const CnetCapsuleCall *last = NULL;
    CnetCSpeakResult wrap;
    unsigned i;

    if (out == NULL) return;
    out->residual_calls = 0;
    out->teacher_calls = 0;
    for (i = 0; i < out->n_calls; ++i) {
        last = &out->calls[i];
        out->calls[i].residual_calls = 0;
        out->calls[i].teacher_calls = 0;
        if (out->calls[i].kind == CNET_CAPSULE_CALL_EXACT)
            last_exact = &out->calls[i];
    }
    if (last_exact != NULL && last_exact->value[0] != '\0') {
        copy_text(out->skill, sizeof out->skill, last_exact->name);
        copy_text(out->value, sizeof out->value, last_exact->value);
        out->bound = 1;
        out->claimed_cert = 1;
        out->kind = CNET_CAPSULE_CALL_EXACT;
        out->refusal[0] = '\0';
        /* Optional C-wrap of last A only if it still contains A. */
        memset(&wrap, 0, sizeof wrap);
        if (cnet_c_speak_after_capsule(last_exact->value, last_exact->name,
                                       &wrap) == 0 &&
            wrap.residual_calls == 0 && wrap.spoken[0] != '\0' &&
            strstr(wrap.spoken, last_exact->value) != NULL) {
            copy_text(out->spoken, sizeof out->spoken, wrap.spoken);
        } else {
            copy_text(out->spoken, sizeof out->spoken, last_exact->value);
        }
        return;
    }
    out->bound = 0;
    out->claimed_cert = 0;
    out->kind = last ? last->kind : CNET_CAPSULE_CALL_ABSTAIN;
    if (last != NULL) {
        copy_text(out->skill, sizeof out->skill, last->name);
        copy_text(out->refusal, sizeof out->refusal, last->refusal);
    } else {
        copy_text(out->refusal, sizeof out->refusal, "ood_no_skill");
    }
    copy_text(out->spoken, sizeof out->spoken, out->refusal);
}

int cnet_capsule_loop_bind_fixture(const char *skill, const char *value,
                                   int verified, CnetCapsuleLoopResult *out)
{
    CnetSkillLaneResult lane;
    CnetCapsulePerm perm;
    CnetCapsuleCallKind kind;

    if (out == NULL) return -1;
    clear_result(out);
    perm = cnet_capsule_loop_pre_execute(CNET_CAPSULE_PERM_ALLOW, skill, NULL,
                                         verified);
    memset(&lane, 0, sizeof lane);
    (void)cnet_skill_lane_bind_fixture(skill, value, verified, &lane);
    if (perm == CNET_CAPSULE_PERM_DENY)
        kind = CNET_CAPSULE_CALL_DENY;
    else if (perm != CNET_CAPSULE_PERM_ALLOW ||
             lane.kind != CNET_SKILL_LANE_EXACT)
        kind = CNET_CAPSULE_CALL_ABSTAIN;
    else
        kind = CNET_CAPSULE_CALL_EXACT;
    append_from_lane(out, skill, &lane, perm, kind);
    speak_last(out);
    return 0;
}

int cnet_capsule_loop_run(const char *turn, const CnetChatLookupTurn *hop,
                          CnetCapsuleLoopResult *out)
{
    char remaining[CNET_CAPSULE_LOOP_TEXT];
    char skill[CNET_SKILL_LANE_NAME];
    char next_name[CNET_SKILL_LANE_NAME];
    CnetCapsulePerm floor;
    unsigned step;

    if (out == NULL) return -1;
    clear_result(out);
    floor = CNET_CAPSULE_PERM_ALLOW;
    next_name[0] = '\0';
    copy_text(remaining, sizeof remaining, turn ? turn : "");

    /* Residual hop: deny/abstain residual_mouth. Never teacher. */
    if (hop != NULL && hop->residual_calls != 0) {
        floor = cnet_capsule_loop_pre_execute(floor, CNET_LOOKUP_CONTRACT, hop,
                                              1);
        append_refuse(out, CNET_LOOKUP_CONTRACT, "residual_mouth", floor,
                      CNET_CAPSULE_CALL_DENY);
        speak_last(out);
        return 0;
    }

    step = 0;
    /* Already-bound lookup hop: reuse skill_lane bind. Never teacher. */
    if (hop != NULL && hop->answered && hop->report.bound &&
        hop->report.value[0] != '\0') {
        CnetSkillLaneResult hop_lane;
        memset(&hop_lane, 0, sizeof hop_lane);
        (void)cnet_skill_lane_bind_lookup(hop, &hop_lane);
        if (hop_lane.kind == CNET_SKILL_LANE_EXACT) {
            append_from_lane(out, CNET_LOOKUP_CONTRACT, &hop_lane,
                             CNET_CAPSULE_PERM_ALLOW, CNET_CAPSULE_CALL_EXACT);
            consume_skill(remaining, CNET_LOOKUP_CONTRACT);
            next_name[0] = '\0';
            if (cnet_skill_lane_route(remaining, next_name,
                                      sizeof next_name) != 0)
                next_name[0] = '\0';
            step = 1;
            if (next_name[0] == '\0') {
                speak_last(out);
                return 0;
            }
        }
    }

    while (step < CNET_CAPSULE_LOOP_MAX_STEPS) {
        CnetSkillLaneResult lane;
        CnetCapsulePerm perm;
        const char *route_src;
        CnetCapsuleCallKind kind;

        route_src = next_name[0] ? next_name : remaining;
        skill[0] = '\0';
        if (cnet_skill_lane_route(route_src, skill, sizeof skill) != 0 ||
            skill[0] == '\0') {
            if (step == 0)
                append_refuse(out, "", "ood_no_skill",
                              CNET_CAPSULE_PERM_ABSTAIN,
                              CNET_CAPSULE_CALL_ABSTAIN);
            break;
        }

        perm = cnet_capsule_loop_pre_execute(floor, skill, hop, 1);
        floor = perm;
        if (perm == CNET_CAPSULE_PERM_DENY) {
            append_refuse(out, skill, "denied", perm, CNET_CAPSULE_CALL_DENY);
            break;
        }
        if (perm == CNET_CAPSULE_PERM_ABSTAIN) {
            append_refuse(out, skill, "ood_no_skill", perm,
                          CNET_CAPSULE_CALL_ABSTAIN);
            break;
        }

        /* Existing increment / crc8 / lookup bind. No second exact-lane law. */
        memset(&lane, 0, sizeof lane);
        (void)cnet_skill_lane_cd_ask(remaining, NULL, &lane);

        if (lane.kind == CNET_SKILL_LANE_EXACT)
            kind = CNET_CAPSULE_CALL_EXACT;
        else
            kind = CNET_CAPSULE_CALL_ABSTAIN;
        append_from_lane(out, skill, &lane, perm, kind);

        if (kind != CNET_CAPSULE_CALL_EXACT)
            break; /* ABSTAIN: stop. never teacher */

        /* EXACT: append A; maybe name next capsule from the SAME turn. */
        consume_skill(remaining, skill);
        next_name[0] = '\0';
        if (cnet_skill_lane_route(remaining, next_name, sizeof next_name) != 0)
            next_name[0] = '\0';
        step++;
        if (next_name[0] == '\0')
            break;
    }

    speak_last(out);
    return 0;
}

int cnet_capsule_loop_cd_ask(const char *turn, const CnetChatLookupTurn *hop,
                             CnetCapsuleLoopResult *out)
{
    return cnet_capsule_loop_run(turn, hop, out);
}
