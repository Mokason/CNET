#ifndef CNET_C_SPEAK_H
#define CNET_C_SPEAK_H

/* Draft mouth — WRAP ONLY.
   C (cce_wordlm_predict) drafts glue. A supplies every asserted slot.
   Teacher / residual / 8B is never the mouth. Not open chat. Not F11. */

#include "cnet_chat_lookup.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_CSPEAK_LEAF "cce_wordlm_predict"
#define CNET_CSPEAK_TEXT 768
#define CNET_CSPEAK_SLOT 160

typedef struct {
    char value[CNET_CSPEAK_SLOT];
    char host[CNET_CSPEAK_SLOT];
    char contract[64];
    char sha256[80];
    int bound; /* 1 = A bound at least the primary value */
} CnetCSpeakSlots;

typedef struct {
    char spoken[CNET_CSPEAK_TEXT];
    int wrapped;             /* 1 = C generated wrap around A */
    int claimed_cert;        /* 1 only if bound && wrapped && value present */
    unsigned residual_calls; /* must stay 0 — residual is never the mouth */
    unsigned leaf_calls;     /* times cce_wordlm_predict ran */
    int may_voice;
    char refusal[80];
} CnetCSpeakResult;

/* Train the tiny wrap leaf (live cce_wordlm). 0 ok, <0 leaf cannot run. */
int cnet_c_speak_init(void);
void cnet_c_speak_shutdown(void);

/* Real generative symbol this mouth calls. */
const char *cnet_c_speak_leaf(void);

/* Hard switch: A bound → wrap A via wordlm; else refuse (no CERT claim).
   Teacher draft is never spoken. */
int cnet_c_speak_wrap(const CnetCSpeakSlots *slots, CnetCSpeakResult *out);

/* Same function tools/cnetd.c:cd_ask calls after a bound lookup hop. */
int cnet_c_speak_after_lookup(const CnetChatLookupTurn *hop,
                              CnetCSpeakResult *out);

/* Same idea for a verified LOCAL/CERT capsule scalar. */
int cnet_c_speak_after_capsule(const char *value, const char *contract,
                               CnetCSpeakResult *out);

/* cd_ask decision: bound A wins; LLM/teacher is never the mouth. */
int cnet_c_speak_cd_ask_step(const CnetCSpeakSlots *bound,
                             const char *teacher_draft, const char *source,
                             int never_voice_llm, CnetCSpeakResult *out);

/* Voice gate. never_voice_llm=1 refuses LLM/teacher. */
int cnet_c_speak_may_voice(const char *source, int never_voice_llm);

/* 1 if answer is a short capsule/slot scalar, not a prose sentence. */
int cnet_c_speak_slot_like(const char *answer);

#ifdef __cplusplus
}
#endif

#endif /* CNET_C_SPEAK_H */
