#ifndef CNET_DC_INVENT_H
#define CNET_DC_INVENT_H

#include "cnet_dc_type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Typed library growth: wake solves I/O, sleep compresses shared sub-bricks.
   Residual is not the mouth. Not an ASI-5 unit. Dream recognition / Q is
   skipped (no second parameter pile). Broader claims WITHHELD. */

#define CNET_DC_INVENT_CONTRACT "dc_invent_v1"
#define CNET_DC_TERM_MAX 96
#define CNET_DC_MAX_PRIMS 32
#define CNET_DC_MAX_HITS 24
#define CNET_DC_MAX_POOL 128
#define CNET_DC_LIST_MAX 16

#define CNET_DC_VAL_NONE 0
#define CNET_DC_VAL_INT 1
#define CNET_DC_VAL_LIST 2
#define CNET_DC_VAL_FN_INCR 3
#define CNET_DC_VAL_FN_CONS 4
#define CNET_DC_VAL_FN_CONS1 5

typedef struct {
    char name[CNET_DC_NAME_MAX];
    int type;
    int invented;
    char body[CNET_DC_TERM_MAX]; /* invented brick body; empty for a base prim */
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

typedef struct {
    int kind;
    long num;
    long items[CNET_DC_LIST_MAX];
    int n_items;
} CnetDcValue;

typedef struct {
    CnetDcValue in;  /* kind NONE => ground term (output-only example) */
    CnetDcValue out;
} CnetDcExample;

void cnet_dc_grammar_init(CnetDcGrammar *g);
int cnet_dc_grammar_add(CnetDcGrammar *g, const char *name, int type,
                        int invented);

void cnet_dc_value_int(CnetDcValue *v, long n);
void cnet_dc_value_list(CnetDcValue *v, const long *items, int n);
int cnet_dc_value_equal(const CnetDcValue *a, const CnetDcValue *b);

/* Evaluate a closed term in the grammar (zero/incr/empty_int/cons + bodies).
   0 ok, 1 refuse, <0 error. */
int cnet_dc_eval_term(const CnetDcGrammar *g, const char *text,
                      CnetDcValue *out);

/* Type-only enumeration. Not a solve. 0 ok (0 hits = abstain). */
int cnet_dc_wake(CnetDcArena *arena, const CnetDcGrammar *g, int goal,
                 int max_depth, CnetDcTerm *hits, int cap, int *n_hits);

/* Wake that solves I/O: a hit must eval-match every example, not merely
   unify with the goal type. n_ex==0 or no match => abstain (0 hits). */
int cnet_dc_wake_io(CnetDcArena *arena, const CnetDcGrammar *g, int goal,
                    int max_depth, const CnetDcExample *ex, int n_ex,
                    CnetDcTerm *hits, int cap, int *n_hits);

/* Register a named brick (used by sleep_compress). Not sleep by itself. */
int cnet_dc_sleep_invent(CnetDcGrammar *g, const char *name,
                         const CnetDcTerm *term);

/* Sleep: extract one shared compound sub-brick that occurs in >=2 solved
   programs and is a proper subterm of at least one. grammar_add of a single
   already-enumerated top-level term is not sleep. 0 invented, 1 nothing. */
int cnet_dc_sleep_compress(CnetDcGrammar *g, const CnetDcTerm *solved,
                           int n_solved, char *name, size_t name_cap,
                           CnetDcTerm *brick);

/* Mine I/O examples from a JSONL of {in,out} numbers. Refuses ASI-5 / CHAT-1
   / compete-suite paths. Missing file => 0 hits, not an error. */
int cnet_dc_mine_io_jsonl(const char *path, CnetDcExample *ex, int cap,
                          int *n_ex);

/* Dream: sample one well-typed term. No residual, no recognition model. */
int cnet_dc_dream(CnetDcArena *arena, const CnetDcGrammar *g, unsigned *rng,
                  int max_depth, CnetDcTerm *out);

/* Speak a bound scalar from a term. Refuses an empty value. */
int cnet_dc_speak_bound(const CnetDcTerm *term, const char *value, char *out,
                        size_t cap);

#ifdef __cplusplus
}
#endif

#endif
