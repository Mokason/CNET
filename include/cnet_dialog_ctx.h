/* Session-scoped dialog context for cnetd (Milestone B).
 *
 * Fixed-cap warm struct: last skill/pack/entities/action.
 * Anaphora ("restart it", "its status") fills slots from last_entities
 * into a rewritten query that still must hit sealed CERT patterns.
 *
 * Law:
 *   - never promotes / never soft-seals
 *   - probe short-circuit bypasses anaphora (callers enforce)
 *   - no malloc on hot path
 *
 * Gate: make dialog_ctx → DIALOG_CTX_PASS
 */
#ifndef CNET_DIALOG_CTX_H
#define CNET_DIALOG_CTX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_DC_ID 64
#define CNET_DC_ENT 96
#define CNET_DC_MAX_ENT 8
#define CNET_DC_Q 512

typedef enum {
    CNET_ACT_NONE = 0,
    CNET_ACT_IDENTITY = 1,
    CNET_ACT_STATUS = 2,
    CNET_ACT_RESTART = 3,
    CNET_ACT_SHOW = 4,
    CNET_ACT_OTHER = 5,
    CNET_ACT_REFUSE = 6
} CnetDialogAction;

typedef struct {
    char last_skill_id[CNET_DC_ID];
    char last_pack_id[CNET_DC_ID];
    char last_entities[CNET_DC_MAX_ENT][CNET_DC_ENT];
    int n_entities;
    CnetDialogAction last_action;
    unsigned turn_seq;
    char last_query[CNET_DC_Q];
    char last_canonical[CNET_DC_Q];
    int last_local; /* 1 if last turn was LOCAL CERT */
} CnetDialogCtx;

typedef struct {
    int applied; /* 1 if rewrite used dialog ctx */
    char reason[48];
    char entity_used[CNET_DC_ENT];
} CnetDialogResolveMeta;

void cnet_dialog_ctx_init(CnetDialogCtx *C);

/* Extract unit/service-like entities from a query into fixed slots. */
int cnet_dialog_extract_entities(const char *query, char ents[][CNET_DC_ENT],
                                 int max_ents);

CnetDialogAction cnet_dialog_infer_action(const char *query);

/* After a completed turn: update last_* from prepared query + skill/pack. */
void cnet_dialog_ctx_update(CnetDialogCtx *C, const char *query_prep,
                            const char *skill_id, const char *pack_id,
                            int was_local);

/* If query has anaphora and ctx has entities, rewrite into out.
 * Returns 1 if rewritten. Never invents entities. */
int cnet_dialog_resolve(const CnetDialogCtx *C, const char *query_in, char *out,
                        size_t cap, CnetDialogResolveMeta *meta);

int cnet_dialog_ctx_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* CNET_DIALOG_CTX_H */
