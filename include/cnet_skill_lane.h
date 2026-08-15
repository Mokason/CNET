#ifndef CNET_SKILL_LANE_H
#define CNET_SKILL_LANE_H

/* AICIMO HARNESS LANE — EXACT NEVER ESCALATES
   Ported law, not AICIMO runtime.
   Model-as-router + deterministic-skills-as-content.
   Exact bind never escalates. OOD abstains. Teacher never speaks.
   Propose != authority: this lane names a capsule; admit/cert is unchanged. */

#include "cnet_chat_lookup.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_SKILL_LANE_TEXT 768
#define CNET_SKILL_LANE_NAME 64
#define CNET_SKILL_LANE_VALUE 160

typedef enum {
    CNET_SKILL_LANE_NONE = 0,
    CNET_SKILL_LANE_EXACT = 1,
    CNET_SKILL_LANE_ABSTAIN = 2
} CnetSkillLaneKind;

typedef struct {
    char skill[CNET_SKILL_LANE_NAME];
    char value[CNET_SKILL_LANE_VALUE];
    char spoken[CNET_SKILL_LANE_TEXT];
    char refusal[80];
    int bound;                 /* 1 = exact A bind */
    int claimed_cert;          /* 1 only if bound */
    unsigned residual_calls;   /* must stay 0 */
    unsigned teacher_calls;    /* must stay 0 — this lane never calls teacher */
    CnetSkillLaneKind kind;
} CnetSkillLaneResult;

/* Hard switch: first dispatch-table skill whose subject appears in the turn.
   Subject must appear; else skill is empty (OOD). No soft mix. */
int cnet_skill_lane_route(const char *turn, char *skill, size_t cap);

/* Bind increment_mod256 / crc8_atm from a byte operand. Never teacher. */
int cnet_skill_lane_bind_exact(const char *skill, unsigned operand,
                               CnetSkillLaneResult *out);

/* Already-bound A slot / lookup-style fixture. verified=1 → LOCAL/CERT. */
int cnet_skill_lane_bind_fixture(const char *skill, const char *value,
                                 int verified, CnetSkillLaneResult *out);

/* Lookup hop that already ran. Binds only if answered && residual_calls==0. */
int cnet_skill_lane_bind_lookup(const CnetChatLookupTurn *hop,
                                CnetSkillLaneResult *out);

/* Route + bind, or abstain. Optional C-wrap. Never calls teacher/residual. */
int cnet_skill_lane_turn(const char *turn, CnetSkillLaneResult *out);

/* Same function tools/cnetd.c:cd_ask calls.
   hop may be NULL. 0 = handled (exact or abstain). Teacher is not reached. */
int cnet_skill_lane_cd_ask(const char *turn, const CnetChatLookupTurn *hop,
                           CnetSkillLaneResult *out);

#ifdef __cplusplus
}
#endif

#endif /* CNET_SKILL_LANE_H */
