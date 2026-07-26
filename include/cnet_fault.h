/* Unified fault economy — one JSONL bus for Ghost/JTC/MCP/salon misses.
 * Cheap adapters and gap_lane both consume this; promote gates audit it.
 *
 * Metadata-only lines always work. Labeled vector lines carry "in":[...] and
 * "tgt":[...] (doubles) so registry_lora_tick can retrain without the in-memory
 * RetrainQueue (cross-process / cross-language capture → native teach). */
#ifndef CNET_FAULT_H
#define CNET_FAULT_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CNET_FAULT_SRC_UNKNOWN = 0,
    CNET_FAULT_SRC_GHOST,
    CNET_FAULT_SRC_JTC,
    CNET_FAULT_SRC_MCP,
    CNET_FAULT_SRC_SALON,
    CNET_FAULT_SRC_SURPRISE,
    CNET_FAULT_SRC_SYNTH
} CnetFaultSource;

typedef struct {
    long long ts_unix;
    CnetFaultSource source;
    char unit[96];
    char skill[96];
    char session[64];
    int in_dim;
    int out_dim;
    char label_kind[32]; /* "argmax" | "residual" | "text" */
    char note[160];
} CnetFaultRecord;

typedef struct {
    char path[512];
    FILE *fp; /* append handle; NULL if closed */
    size_t append_count;
} CnetFaultLog;

/* path NULL => CNET_FAULT_LOG or "./cnet_faults.jsonl" */
int cnet_fault_open(CnetFaultLog *log, const char *path);
void cnet_fault_close(CnetFaultLog *log);

/* Append one metadata line (JSON object, single line). Returns 0 or -1. */
int cnet_fault_append(CnetFaultLog *log, const CnetFaultRecord *rec);

/* Append metadata + input/target vectors (dims from rec->in_dim/out_dim).
 * in/tgt may be NULL for metadata-only (same as cnet_fault_append). */
int cnet_fault_append_labeled(CnetFaultLog *log, const CnetFaultRecord *rec,
                              const double *in, const double *tgt);

/* Count lines in path (re-open read-only). */
size_t cnet_fault_count_file(const char *path);

/* Load up to cap metadata records matching unit (NULL/empty => all). */
size_t cnet_fault_load(const char *path, const char *unit_filter,
                       CnetFaultRecord *out, size_t cap);

/* Load labeled vector pairs for unit into caller buffers.
 * inputs: cap * in_dim, targets: cap * out_dim. Returns n pairs loaded.
 * Skips lines without in/tgt or dim mismatch. in_dim/out_dim must match rec. */
size_t cnet_fault_load_vectors(const char *path, const char *unit,
                               int in_dim, int out_dim,
                               double *inputs, double *targets, size_t cap);

const char *cnet_fault_source_name(CnetFaultSource s);
CnetFaultSource cnet_fault_source_parse(const char *s);

/* ---- dual-write mirror (linked when this TU is in the binary) ------------ */
/* Called from registry_supply_label when CNET_FAULT_MIRROR is unset or "1".
 * Writes a labeled JTC/ghost line to CNET_FAULT_LOG if set. Safe no-op if log
 * unset. Strong symbol — link cnet_fault.o to enable. */
void cnet_fault_mirror_labeled(const char *unit, const double *input,
                               const double *target, int in_dim, int out_dim,
                               const char *source_name);

/* As above, but the caller names label_kind ("argmax" | "residual" | "text")
 * and note. Used by the Tier-C organic capture so a residual-taught row is
 * distinguishable from a synthetic seeder row by provenance alone. NULL/empty
 * label_kind or note fall back to the mirror_labeled defaults. */
void cnet_fault_mirror_kind(const char *unit, const double *input,
                            const double *target, int in_dim, int out_dim,
                            const char *source_name, const char *label_kind,
                            const char *note);

#ifdef __cplusplus
}
#endif
#endif
