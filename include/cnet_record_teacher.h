/* Record-as-oracle teacher: certify specialists against RECORDED text instead
 * of a language model. This is the seam where learning stops being bounded by
 * teachers — the record (a user's correction, a verified output) is ground
 * truth the LM never produced, and the sealed unit provably reproduces it.
 *
 * A record is plain text at <records_dir>/<goal_tag>.txt. Its in-window word
 * transitions define a total function over the window vocabulary:
 *   next(i) = window index of the word following word i in the record
 *             (first occurrence wins; out-of-window words are skipped over),
 *   next(i) = i (identity) for words the record says nothing about.
 * Identity fallback is load-bearing twice over: the teacher never abstains
 * (>~10% abstention trips the oracle_unfit gate) and the answer set stays
 * per-point distinct (a constant fallback would trip class_imbalance in
 * sampled mode). The domain is fully enumerable (ONEHOT[W]), so mining is
 * exhaustive and certification can reach CERT_PROVEN — proof, not hope,
 * against the record bytes. */
#ifndef CNET_RECORD_TEACHER_H
#define CNET_RECORD_TEACHER_H

#include <stddef.h>

#include "cnet_export.h"
#include "gap_lane.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Gaps this teacher binds: goal tags with this prefix (note_skill output for
 * consolidated corrections). */
#define CNET_RECORD_TAG_PREFIX "skill_corr_"

#define CNET_RECORD_W_MAX 4096   /* matches CNET_AUTO_LEARN_W clamp ceiling */
#define CNET_RECORD_WORD_MAX 32
#define CNET_RECORD_MAX 32       /* record oracles per tick (ACQUIRE_MAX_ORACLES bound) */
#define CNET_RECORD_TEXT_MAX 65536

/* Teacher context: the compiled transition table for one record. */
typedef struct {
    int w;                          /* window size (input one-hot width) */
    int k;                          /* goal fields; answers chain next() k times */
    int next[CNET_RECORD_W_MAX];    /* successor window index (identity default) */
} CnetRecordCtx;

/* Loads the window words sidecar ("<id><ws><word>" per line, window order).
 * Returns the word count, or -1 on unreadable/oversized file. */
CNET_API int cnet_record_words_load(const char *words_path,
                                    char (*words)[CNET_RECORD_WORD_MAX],
                                    size_t cap);

/* Compiles record text into a transition table over the given word list.
 * Tokenization: lowercase ASCII letter runs; anything else separates.
 * Returns the number of in-window transitions recorded (0 is legal — the
 * table is then pure identity). */
CNET_API int cnet_record_table_build(CnetRecordCtx *ctx, int w, int k,
                                     const char *text,
                                     char (*words)[CNET_RECORD_WORD_MAX]);

/* CnetOracleFn: answers every point, never abstains. Field r of the goal is
 * next() applied r+1 times from the input word (deterministic chain). */
CNET_API int cnet_record_teacher(const double *in, double *out, void *ctx);

/* Binds record oracles for every candidate skill_corr_* gap whose record
 * file exists. Call once per tick AFTER the registry rebuild (the registry
 * is memset every tick; contexts live in a static pool repopulated here).
 * toolchain_fp: the lane's toolchain fingerprint (0 = builtin fallback).
 * Returns the number of oracles bound. */
CNET_API size_t cnet_record_bind(GapLane *L, const char *records_dir,
                                 const char *words_path,
                                 unsigned long long toolchain_fp);

#ifdef __cplusplus
}
#endif
#endif /* CNET_RECORD_TEACHER_H */
