#ifndef CNET_GROK_GUIDE_H
#define CNET_GROK_GUIDE_H

/* Optional Grok guide during evolve — NOT a CERT path.
 *
 * Answers a few guide questions (from guide_questions.txt or harvest)
 * with a hard wall-clock cap per question (default 30 minutes).
 * Writes prose to guide_log.md only. Never auto-CERT / never mouth.
 *
 * Env:
 *   CNET_GROK_GUIDE=1
 *   CNET_GROK_GUIDE_MAX_Q=3
 *   CNET_GROK_GUIDE_SECONDS=1800   (per question)
 *   XAI_API_KEY / XAI_KEY / GROK_API_KEY
 *   CNET_GROK_MODEL (default grok-4-1-fast-reasoning or grok-3)
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int enabled;
    int asked;
    int answered;
    int skipped;
    int errors;
    int timed_out;
    double seconds_limit;
    char log_path[512];
} CnetGrokGuideReport;

/* Run up to max_q questions from bricks_dir/guide_questions.txt.
 * seconds_per_q hard cap (wall clock). Returns 0 if ran. */
int cnet_grok_guide_run(const char *bricks_dir, int max_q, int seconds_per_q,
                        CnetGrokGuideReport *rep);

#ifdef __cplusplus
}
#endif

#endif
