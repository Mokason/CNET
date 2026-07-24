/* Unified fault economy — one JSONL bus for Ghost/JTC/MCP/salon misses.
 * Cheap adapters and gap_lane both consume this; promote gates audit it. */
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

/* Count lines in path (re-open read-only). */
size_t cnet_fault_count_file(const char *path);

/* Load up to cap records matching unit (empty unit => all). Returns n loaded. */
size_t cnet_fault_load(const char *path, const char *unit_filter,
                       CnetFaultRecord *out, size_t cap);

const char *cnet_fault_source_name(CnetFaultSource s);
CnetFaultSource cnet_fault_source_parse(const char *s);

#ifdef __cplusplus
}
#endif
#endif
