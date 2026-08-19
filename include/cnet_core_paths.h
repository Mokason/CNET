#ifndef CNET_CORE_PATHS_H
#define CNET_CORE_PATHS_H

/* Four deep product paths for AGI-like CORE (not a mind):
 *
 *  1) LIVE WAIST   — CERT/table RESULT or ABSTAIN only (no leftover mouth)
 *  2) BRICK FACTORY— multi-domain 4-verb; N+1 only after N serves w/o LLM
 *  3) MISS→ADMIT   — miss-log propose → TABLE ≥0.95 → specialist_admit
 *  4) COMPOSE      — split oversized / compose parked bricks
 *
 * Benchmarks: make cnet_path1_waist cnet_path2_factory
 *             cnet_path3_missadmit cnet_path4_compose
 *             cnet_core_paths (all)
 */

#include "cnet_core_bus.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_PATH_MAX_BRICKS 8
#define CNET_PATH_TAG 32
#define CNET_PATH_NAME 64

typedef struct {
    int cert_hits;
    int abstains;
    int open_chat_blocked;
    int roe_blocked;
    int llm_blocked;
    double ms_total;
} CnetPath1Bench;

typedef struct {
    int n_requested;
    int n_built;
    int n_served_ok; /* bricks that proved full domain 16/16 */
    int skipped_for_gate; /* N+1 refused before N served */
    double ms_per_brick[CNET_PATH_MAX_BRICKS];
    double ms_total;
    char tags[CNET_PATH_MAX_BRICKS][CNET_PATH_TAG];
} CnetPath2Bench;

typedef struct {
    int propose_rc;
    int proposed;
    int table_ok;
    int admit_ok;
    int served_ok;
    int admit_calls_delta; /* must stay 0 during propose */
    double ms_propose;
    double ms_table_admit;
    double ms_total;
    char proposed_name[CNET_PATH_NAME];
} CnetPath3Bench;

typedef struct {
    int split_ok;
    int compose_ok;
    int child_a_ok;
    int child_b_ok;
    int composed_ok;
    int n_plus_one_blocked;
    double ms_split;
    double ms_compose;
    double ms_total;
} CnetPath4Bench;

/* ---- Path 1: live waist decision (pure, no I/O) ----
 * Returns: 0 = CERT prove path, 1 = abstain, 2 = blocked illegal mouth */
int cnet_path1_waist_decide(int has_cert_result, int open_chat_attempt,
                            int roe_llm_attempt, int *out_action);
/* out_action: 0=emit_cert, 1=abstain, 2=kill_mouth */

int cnet_path1_bench(CnetPath1Bench *b);

/* ---- Path 2: brick factory ----
 * Builds domains in order; refuses next until current proves 16/16. */
typedef struct {
    const char *tensor;
    const char *tag;
    const char *name;
    int mode; /* 0 add, 1 xor */
} CnetPath2Spec;

int cnet_path2_factory_run(CnetCoreBus *bus, const char *bonsai,
                           const char *bricks_dir, const CnetPath2Spec *specs,
                           int n_specs, CnetPath2Bench *b);

int cnet_path2_bench(CnetPath2Bench *b);

/* ---- Path 3: miss-log → propose → table ≥0.95 → admit ----
 * Builds a domain GGUF from certified miss-log nibble pairs when present;
 * otherwise proposes via e-graph only (no admit). When pairs exist, full admit. */
int cnet_path3_miss_to_admit(CnetCoreBus *bus, const char *miss_path,
                             const char *domain_gguf, const char *brick_name,
                             const char *domain_tag, CnetPath3Bench *b);

int cnet_path3_bench(CnetPath3Bench *b);

/* Write miss-log JSONL with 16 certified nibble I/O pairs for path3. */
int cnet_path3_write_nibble_misslog(const char *path, const float lut[16],
                                    const char *goal_type);

/* ---- Path 4: split / compose ----
 * split: one 16-domain brick → two 8-half tags (lo/hi nibble halves as 0..7 mapped)
 * compose: sequential apply tag_a then tag_b on same nibble (pipeline)
 * n_plus_one_blocked: trying factory next before serve returns refuse */
int cnet_path4_split_brick(CnetCoreBus *bus, const char *src_tag,
                           const char *tag_lo, const char *tag_hi,
                           CnetPath4Bench *b);
int cnet_path4_compose_bricks(CnetCoreBus *bus, const char *tag_a,
                              const char *tag_b, const char *tag_out,
                              CnetPath4Bench *b);
int cnet_path4_bench(CnetPath4Bench *b);

#ifdef __cplusplus
}
#endif

#endif /* CNET_CORE_PATHS_H */
