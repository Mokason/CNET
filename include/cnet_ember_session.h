#ifndef CNET_EMBER_SESSION_H
#define CNET_EMBER_SESSION_H

/* Ember multi-turn residual session: session_sync extend vs rebuild.
 * Compact keeps durable summary + recent tail; summary never seals CERT.
 */

#include "cnet_ember_ckpt.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_EMBER_TX_MAX (256 * 1024)
#define CNET_EMBER_SUMMARY_MAX 4096
#define CNET_EMBER_TURN_MAX 8192

typedef enum {
    CNET_EMBER_SYNC_EXTEND = 0,
    CNET_EMBER_SYNC_REBUILD = 1,
    CNET_EMBER_SYNC_LOAD_CKPT = 2,
    CNET_EMBER_SYNC_EMPTY = 3
} CnetEmberSyncKind;

typedef struct {
    CnetEmberSyncKind kind;
    int common_chars;
    int compacted;
    int claimed_cert; /* always 0 */
} CnetEmberSyncResult;

typedef struct {
    int drafted;
    int compacted;
    int steered;
    int from_ckpt;
    int claimed_cert; /* always 0 */
    char reason[48];
} CnetEmberDraftReport;

typedef struct {
    char id[64];
    char *tx; /* owned transcript */
    size_t tx_len;
    size_t tx_cap;
    char summary[CNET_EMBER_SUMMARY_MAX];
    int n_turns;
    int compacted_n;
    char prefix_sha[CNET_EMBER_SHA_HEX];
    int soft_compact_chars; /* default 48k */
    int hard_compact_chars; /* default 96k */
    int tail_keep_chars;    /* default 8k */
    int open;
} CnetEmberSession;

void cnet_ember_session_init(CnetEmberSession *s);
void cnet_ember_session_free(CnetEmberSession *s);

/* Synchronize session to full rendered prompt (or single user turn).
 * If full_prompt starts with current transcript → EXTEND (append suffix).
 * Else try ckpt prefix load → LOAD_CKPT then append remainder.
 * Else REBUILD from full_prompt. */
int cnet_ember_session_sync(CnetEmberSession *s, CnetEmberCkptStore *ckpt,
                            const char *full_prompt, CnetEmberSyncResult *out);

/* Append one user/assistant pair (rendered). */
int cnet_ember_session_append_pair(CnetEmberSession *s, const char *user,
                                   const char *assistant);

/* Soft/hard compact. Summary is local durable text — never gold/CERT.
 * If held_summarize is NULL, uses extractive head+tail template. */
typedef int (*CnetEmberSummarizeFn)(const char *body, size_t len, char *out,
                                    size_t cap, void *ud);
int cnet_ember_session_maybe_compact(CnetEmberSession *s, CnetEmberCkptStore *ckpt,
                                     CnetEmberSummarizeFn fn, void *ud,
                                     int force);

/* Checkpoint current transcript. */
int cnet_ember_session_checkpoint(CnetEmberSession *s, CnetEmberCkptStore *ckpt,
                                  CnetEmberCkptReason reason);

#ifdef __cplusplus
}
#endif

#endif /* CNET_EMBER_SESSION_H */
