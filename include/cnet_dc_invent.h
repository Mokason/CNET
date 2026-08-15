#ifndef CNET_DC_INVENT_H
#define CNET_DC_INVENT_H

#include "cnet_dc_type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DreamCoder wake / sleep / dream / speak for Core.
   Typed invention only. Residual is not the mouth. Not an ASI-5 unit. */

#define CNET_DC_INVENT_CONTRACT "dc_invent_v1"
#define CNET_DC_TERM_MAX 96
#define CNET_DC_MAX_PRIMS 32
#define CNET_DC_MAX_HITS 24
#define CNET_DC_MAX_POOL 128

typedef struct {
    char name[CNET_DC_NAME_MAX];
    int type;
    int invented;
} CnetDcPrim;

typedef struct {
    CnetDcPrim prims[CNET_DC_MAX_PRIMS];
    int n_prims;
} CnetDcGrammar;

typedef struct {
    char text[CNET_DC_TERM_MAX];
    int type;
    int depth;
    int used_invented;
} CnetDcTerm;

void cnet_dc_grammar_init(CnetDcGrammar *g);
int cnet_dc_grammar_add(CnetDcGrammar *g, const char *name, int type,
                        int invented);

/* Wake: enumerate undeclared well-typed terms for goal. 0 ok (0 hits = abstain). */
int cnet_dc_wake(CnetDcArena *arena, const CnetDcGrammar *g, int goal,
                 int max_depth, CnetDcTerm *hits, int cap, int *n_hits);

/* Sleep: register a hit as an invented primitive. Different later goals
   may reuse it at lower depth. */
int cnet_dc_sleep_invent(CnetDcGrammar *g, const char *name,
                         const CnetDcTerm *term);

/* Dream: sample one well-typed term. No residual. 0 ok, 1 empty grammar. */
int cnet_dc_dream(CnetDcArena *arena, const CnetDcGrammar *g, unsigned *rng,
                  int max_depth, CnetDcTerm *out);

/* Speak a bound scalar from a term. Refuses an empty value. */
int cnet_dc_speak_bound(const CnetDcTerm *term, const char *value, char *out,
                        size_t cap);

#ifdef __cplusplus
}
#endif

#endif
