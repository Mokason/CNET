#ifndef CNET_CHAT_LOOKUP_H
#define CNET_CHAT_LOOKUP_H

#include "cnet_lookup.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Conversation hop: offer a URL from a chat turn to web_lookup_v1.
   Live caller is cnetd (cnet_chat_lookup_cnetd_hop), not fluency/compete.

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
    /* Times this hop used residual/utter as the mouth. Must stay 0. */
    unsigned residual_calls;
} CnetChatLookupTurn;

/* First URL-like token in the turn. Returns 1 if found. Does not invent. */
int cnet_chat_lookup_offer_url(const char *turn, char *url, size_t cap);

/* Named bind kind in the turn, else CNET_LOOKUP_BIND_INTEGER. */
CnetLookupBind cnet_chat_lookup_infer_bind(const char *turn);

/* Production hop (no file://). 0 spoken bound value, 1 abstain, <0 error.
   No URL / bad scheme / blocked host / fetch fail / unbindable → abstain.
   Residual is never the mouth. */
int cnet_chat_lookup_turn(const char *turn, CnetChatLookupTurn *out);

/* Same hop with flags. CNET_LOOKUP_F_ALLOW_FILE is test-only. */
int cnet_chat_lookup_turn_flags(const char *turn, unsigned flags,
                                CnetChatLookupTurn *out);

/* Same function tools/cnetd.c:cd_ask calls before residual/teacher/ROE
   lookup. Production flags. 0 = bound+spoken; 1 = fall through. */
int cnet_chat_lookup_cnetd_hop(const char *turn, CnetChatLookupTurn *out);

#ifdef __cplusplus
}
#endif

#endif
