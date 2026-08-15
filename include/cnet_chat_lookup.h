#ifndef CNET_CHAT_LOOKUP_H
#define CNET_CHAT_LOOKUP_H

#include "cnet_lookup.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Conversation hop: offer a URL from a chat turn to web_lookup_v1.
   Not a compete unit. Not residual speech. Not a Tier-A training target.

   Bind default is integer: CHAT contracts already speak one unsigned
   scalar (increment, minutes, crc, policy, compose), and the lookup
   CLI defaults to --bind integer. The turn may name token/line/year. */

typedef struct {
    char url[512];
    CnetLookupBind bind;
    int offered;
    int answered;
    char spoken[512];
    char refusal[80];
    CnetLookupReport report;
} CnetChatLookupTurn;

/* First URL-like token in the turn. Returns 1 if found. Does not invent. */
int cnet_chat_lookup_offer_url(const char *turn, char *url, size_t cap);

/* Named bind kind in the turn, else CNET_LOOKUP_BIND_INTEGER. */
CnetLookupBind cnet_chat_lookup_infer_bind(const char *turn);

/* 0 spoken bound value, 1 abstain, <0 error.
   No URL / bad scheme / fetch fail / unbindable → abstain and do not speak.
   Residual is never the mouth. */
int cnet_chat_lookup_turn(const char *turn, CnetChatLookupTurn *out);

#ifdef __cplusplus
}
#endif

#endif
