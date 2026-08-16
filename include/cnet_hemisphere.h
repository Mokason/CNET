#ifndef CNET_HEMISPHERE_H
#define CNET_HEMISPHERE_H

/* CORE is the MIDDLE GROUND — not AGI competence.
 *
 * CORE's job is "what is what":
 *   CERT plane  = logic-strong  (exact skills, math, wiki, capsules)
 *   OPEN CHAT   = creativity-strong (held / MAX / Bonsai drafts)
 *
 *   turn ──► CORE discern (logic vs creative)
 *              │
 *      ┌───────┴────────┐
 *      ▼                ▼
 *  CERT (logic)    OPEN CHAT (creative)
 *  claimed_cert=1  claimed_cert=0 always
 *
 * Law:
 *   - CORE is not AGI; it arbitrates planes
 *   - CERT wins when it can bind (even on creative-looking turns)
 *   - pure LOGIC miss abstains by default (no creative fill-in)
 *   - CREATIVE leftover may use open chat (never auto-CERT)
 *   - voice: CERT local ok; open chat gated (default off)
 *
 * Gate: make cnet_hemi → CNET_HEMI_PASS
 */

#include "cnet_skill_lane.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which side of CORE produced the bind (not “is CORE”). */
typedef enum {
    CNET_HEMI_CORE = 0,     /* CERT plane (legacy name kept) */
    CNET_HEMI_RESIDUAL = 1, /* OPEN CHAT plane (legacy name) */
    CNET_HEMI_NONE = 2
} CnetHemiId;

/* Explicit plane tags (preferred over hemi id in new code). */
typedef enum {
    CNET_CORE_PLANE_NONE = 0,
    CNET_CORE_PLANE_CERT = 1,      /* logic-strong claimed data */
    CNET_CORE_PLANE_OPEN_CHAT = 2  /* creativity-strong mind draft */
} CnetCorePlane;

/* CORE discern: what kind of ask is this? */
typedef enum {
    CNET_CORE_INTENT_UNKNOWN = 0,
    CNET_CORE_INTENT_LOGIC = 1,    /* prefer CERT */
    CNET_CORE_INTENT_CREATIVE = 2, /* prefer open chat after CERT try */
    CNET_CORE_INTENT_MIXED = 3
} CnetCoreIntent;

typedef enum {
    CNET_HEMI_SRC_NONE = 0,
    CNET_HEMI_SRC_SKILL_EXACT = 1,
    CNET_HEMI_SRC_LOOKUP = 2,
    CNET_HEMI_SRC_OOD_MATH = 3,
    CNET_HEMI_SRC_OOD_WIKI = 4,
    CNET_HEMI_SRC_HELD_LLM = 5, /* open-chat held */
    CNET_HEMI_SRC_ABSTAIN = 6,
    CNET_HEMI_SRC_OPEN_CHAT = 5 /* alias of HELD_LLM */
} CnetHemiSource;

typedef struct {
    int never_voice_llm;     /* 1 = default: open-chat drafts not voiced */
    int residual_enabled;    /* 1 = allow open-chat plane when appropriate */
    int open_chat_enabled;   /* same bit as residual_enabled (mirror) */
    int allow_wiki;          /* 1 = CERT plane may use wiki hop */
    int open_chat_may_voice; /* 1 = allow TTS for open-chat (rare) */
    /* 0 (default): pure LOGIC miss does not fall into creative open chat */
    int logic_open_chat_fallback;
} CnetHemiPolicy;

typedef struct {
    int via_core; /* 1 if CORE middle handled the turn */
    CnetCoreIntent intent;   /* discern: logic / creative / mixed */
    CnetCorePlane plane;     /* CERT | OPEN_CHAT | NONE */
    CnetHemiId hemi;         /* legacy: CORE=CERT, RESIDUAL=OPEN_CHAT */
    CnetHemiSource source;
    int bound;
    int claimed_cert; /* 1 only CERT plane — never open chat */
    int may_voice;
    int open_chat; /* 1 if plane == OPEN_CHAT */
    unsigned residual_calls;
    unsigned teacher_calls;
    char skill[CNET_SKILL_LANE_NAME];
    char value[CNET_SKILL_LANE_VALUE];
    char spoken[CNET_SKILL_LANE_TEXT];
    char refusal[80];
} CnetHemiResult;

void cnet_hemi_policy_default(CnetHemiPolicy *p);

const char *cnet_hemi_name(CnetHemiId id);
const char *cnet_hemi_source_name(CnetHemiSource s);
const char *cnet_core_plane_name(CnetCorePlane p);
const char *cnet_core_intent_name(CnetCoreIntent i);

/* Discern "what is what" — heuristic, no LLM (CORE stays cheap). */
CnetCoreIntent cnet_core_discern(const char *turn);

/* Map a skill-lane row into CORE tags (no I/O). */
void cnet_hemi_classify_lane(const CnetSkillLaneResult *lane, int never_voice_llm,
                             CnetHemiResult *out);

int cnet_hemi_may_voice(const CnetHemiResult *r, int never_voice_llm);

/* CORE middle ask: discern + CERT (+ open chat per intent/policy). */
int cnet_hemi_ask(const char *turn, const CnetHemiPolicy *policy,
                  CnetHemiResult *out);

int cnet_core_ask(const char *turn, const CnetHemiPolicy *policy,
                  CnetHemiResult *out);

/* CERT plane only (no open chat). Used by capsule_loop. */
int cnet_hemi_ask_core(const char *turn, const CnetHemiPolicy *policy,
                       CnetHemiResult *out);

int cnet_core_ask_cert(const char *turn, const CnetHemiPolicy *policy,
                       CnetHemiResult *out);

#ifdef __cplusplus
}
#endif

#endif /* CNET_HEMISPHERE_H */
