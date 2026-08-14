#ifndef CNET_CHAT_FLUENCY_H
#define CNET_CHAT_FLUENCY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Named frozen dialogue rubric. Not Elo. Not LLM-as-judge.
   Beat-8B on this rubric stays WITHHELD until a compete publishes
   both CNET and Bonsai scores. */

#define CNET_CHAT_FLUENCY_V1_NAME "cnet_chat_fluency_v1"
#define CNET_CHAT_FLUENCY_V1_AXES 5

typedef struct {
    char contract[64];
    char input[32];
    char output[32];
} CnetChatFact;

typedef struct {
    int answered;
    const char *when;
    const char *contract;
    const char *input;
    const char *output;
    const CnetChatFact *prior;
    size_t prior_count;
} CnetChatFluencyQuery;

typedef struct {
    float well_formed;
    float stay_on_contract;
    float no_contradiction;
    float bounded_helpfulness;
    float no_side_effect;
    float overall;
    int valid;
} CnetChatFluencyScore;

void cnet_chat_fluency_v1_score(const char *spoken,
                                const CnetChatFluencyQuery *query,
                                CnetChatFluencyScore *score_out);

/* 1 if every axis is at least floor and the score is valid. */
int cnet_chat_fluency_v1_pass(const CnetChatFluencyScore *score, float floor);

#ifdef __cplusplus
}
#endif

#endif
