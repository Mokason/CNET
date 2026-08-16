#ifndef CNET_LIVE_MISS_H
#define CNET_LIVE_MISS_H

/* Live structured miss capture + domain completion for self-evolve.
 *
 * Typed JSONL rows (one domain table grows until 16/16 outs known):
 *   {"via":"typed_miss","domain":"TAG","in":3,"out":5,"has_in":1,"has_out":1,
 *    "certified":1,"goal_type":"TAG"}
 *
 * Teach forms (out known):
 *   "teach TAG 3 5" | "TAG 3 = 5" | "fill TAG slot=3 out=5"
 * Query miss (in only):
 *   "TAG 3"  when not yet served
 *
 * Gate helper used by evolve + tests.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse "TAG n" → 0. Returns -1 if not that shape. */
int cnet_live_parse_tag_n(const char *turn, char *tag_out, size_t tag_cap,
                          unsigned *n_out);

/* Parse teach forms → domain,in,out. 0 ok. */
int cnet_live_parse_teach(const char *turn, char *tag_out, size_t tag_cap,
                          unsigned *in_out, unsigned *out_v);

/* Append typed miss row. has_out=0 if out unknown. certified=1 only if has_out. */
int cnet_live_miss_append(const char *miss_path, const char *domain, unsigned in_n,
                          int has_out, unsigned out_n);

/* Count complete (has out) unique inputs for domain. Returns 0..16. */
int cnet_live_miss_domain_pairs(const char *miss_path, const char *domain,
                                float lut[16]);

/* List domains in miss_log that now have 16/16 pairs. Returns count.
 * domains[][] each CNET_LIVE_DOM_MAX. */
#define CNET_LIVE_DOM_MAX 32
#define CNET_LIVE_DOM_NAME 32
int cnet_live_miss_complete_domains(const char *miss_path,
                                    char domains[][CNET_LIVE_DOM_NAME], int max_dom);

#ifdef __cplusplus
}
#endif

#endif /* CNET_LIVE_MISS_H */
