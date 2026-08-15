#ifndef CNET_CAPSULE_LOOP_H
#define CNET_CAPSULE_LOOP_H

/* CAPSULE LOOP — BOUNDED CALL LOG
   Steal DeepSeek loop shape, not their runtime.
   Exact never escalates. OOD abstains. Not open chat, not F11, not general mind.
   WITHHELD.

   Hard-switch route (cnet_skill_lane_route) -> monotonic allow/abstain/deny
   -> existing increment / crc8 / lookup bind -> speak last A.
   residual_calls == 0 && teacher_calls == 0. Teacher never speaks. */

#include "cnet_chat_lookup.h"
#include "cnet_skill_lane.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_CAPSULE_LOOP_MAX_STEPS 2
#define CNET_CAPSULE_LOOP_TEXT 768
#define CNET_CAPSULE_LOOP_NAME 64
#define CNET_CAPSULE_LOOP_VALUE 160

typedef enum {
    CNET_CAPSULE_PERM_ALLOW = 0,
    CNET_CAPSULE_PERM_ABSTAIN = 1,
    CNET_CAPSULE_PERM_DENY = 2
} CnetCapsulePerm;

typedef enum {
    CNET_CAPSULE_CALL_NONE = 0,
    CNET_CAPSULE_CALL_EXACT = 1,
    CNET_CAPSULE_CALL_ABSTAIN = 2,
    CNET_CAPSULE_CALL_DENY = 3
} CnetCapsuleCallKind;

typedef struct {
    char name[CNET_CAPSULE_LOOP_NAME];
    char value[CNET_CAPSULE_LOOP_VALUE];
    char call_id[16];
    char refusal[80];
    CnetCapsuleCallKind kind;
    CnetCapsulePerm perm;
    int bound;
    int claimed_cert;          /* 1 only if bound */
    unsigned residual_calls;   /* must stay 0 */
    unsigned teacher_calls;    /* must stay 0 — this lane never calls teacher */
} CnetCapsuleCall;

typedef struct {
    CnetCapsuleCall calls[CNET_CAPSULE_LOOP_MAX_STEPS];
    unsigned n_calls;
    unsigned n_exact;
    char skill[CNET_CAPSULE_LOOP_NAME];
    char value[CNET_CAPSULE_LOOP_VALUE];
    char spoken[CNET_CAPSULE_LOOP_TEXT];
    char refusal[80];
    int bound;
    int claimed_cert;          /* 1 iff last EXACT bind */
    unsigned residual_calls;   /* must stay 0 */
    unsigned teacher_calls;    /* must stay 0 */
    CnetCapsuleCallKind kind;
} CnetCapsuleLoopResult;

/* Monotonic pre-execute. Deny cannot become allow. No ask-the-8B. */
CnetCapsulePerm cnet_capsule_loop_pre_execute(CnetCapsulePerm prior,
                                              const char *name,
                                              const CnetChatLookupTurn *hop,
                                              int verified_fixture);

/* How many hard-table subjects the turn still names. Reuses route. */
int cnet_capsule_loop_count_subjects(const char *turn);

/* Already-bound fixture. verified=0 → abstain. Never teacher. */
int cnet_capsule_loop_bind_fixture(const char *skill, const char *value,
                                   int verified, CnetCapsuleLoopResult *out);

/* Bounded loop. hop may be NULL. 0 = handled (exact, abstain, or deny). */
int cnet_capsule_loop_run(const char *turn, const CnetChatLookupTurn *hop,
                          CnetCapsuleLoopResult *out);

/* Same function tools/cnetd.c:cd_ask calls when the turn names two skills.
   0 = handled. Teacher is not reached. */
int cnet_capsule_loop_cd_ask(const char *turn, const CnetChatLookupTurn *hop,
                             CnetCapsuleLoopResult *out);

#ifdef __cplusplus
}
#endif

#endif /* CNET_CAPSULE_LOOP_H */
