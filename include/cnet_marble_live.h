/* Marble Live wiring: episodic path, chat actions, residual stage draft.
 *
 * Split out of cnetd.c so the law-bearing bits are unit-testable without a
 * running daemon.  Gate: make marble_live -> MARBLE_LIVE_PASS
 *
 * Law (docs/MARBLE_LIVE.md):
 *   - propose != admit: chat can PROPOSE a capsule, never seal one
 *   - the residual stage is a DRAFT beside the answer, never the answer, and
 *     never carries the LOCAL source name
 *   - actions are a closed allowlist; chat never reaches a general shell
 */
#ifndef CNET_MARBLE_LIVE_H
#define CNET_MARBLE_LIVE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_ML_PATH 512
#define CNET_ML_UNIT 64
#define CNET_ML_TEXT 1024

/* Durable episodic log. CNET_EPISODIC_PATH wins; else
 * $CNET_MINIMAL_ROOT/var/marble_episodic.jsonl; else <fallback_root>/var/... .
 * Creates the parent directory. Returns 0 on success. */
int cnet_ml_episodic_path(char *out, size_t cap, const char *fallback_root);

/* Strict allowlist for anything that reaches a command line: [a-z0-9_-], other
 * runs collapse to '_', length capped. Returns 0 if a usable name survived.
 * Chat is untrusted input; this is the only door to argv. */
int cnet_ml_sanitize_unit(const char *in, char *out, size_t cap);

/* "propose capsule for gap health" -> "gap_health". Returns 0 if found. */
int cnet_ml_extract_unit(const char *query, char *out, size_t cap);

/* Run bin/cnet_capsule_propose --unit <unit>. Propose only: the tool writes a
 * pending row under var/capsule_inbox and never seals. Returns 0 on success
 * and fills `out` with the tool's summary line. */
int cnet_ml_propose_capsule(const char *unit, char *out, size_t cap);

/* Residual stage draft, ONLY meaningful after a CERT miss.
 * Returns 1 when a draft was produced, 0 when disabled/unavailable.
 * Off unless CNET_STAGE_RESIDUAL=1. Never returns anything the caller may
 * present as CERT. */
int cnet_ml_stage_enabled(void);
int cnet_ml_stage_draft(const char *query, char *out, size_t cap);
/* Same as stage_draft, with optional retrieved CNET context (never CERT). */
int cnet_ml_stage_draft_ctx(const char *query, const char *ctx, char *out, size_t cap);
/* Pack overlapping ingest + english catalog snippets. claimed_cert stays 0. */
int cnet_ml_context_pack(const char *query, char *out, size_t cap);

int cnet_ml_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* CNET_MARBLE_LIVE_H */
