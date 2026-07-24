#ifndef CNET_SERVE_DECODE_H
#define CNET_SERVE_DECODE_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Map top-k pick indices into a human-readable line using optional labels.
 * labels may be NULL => "idx0,idx1,...". Writes into out (NUL-terminated).
 * Returns bytes written (excl NUL) or -1. */
int cnet_serve_decode_picks(const int *picks, size_t n_picks,
                            const char *const *labels, size_t n_labels,
                            char *out, size_t out_cap);
/* JSON-ish one liner for MCP: {"picks":[...],"text":"..."} */
int cnet_serve_decode_json(const int *picks, size_t n_picks,
                           const char *const *labels, size_t n_labels,
                           char *out, size_t out_cap);

/* Decode using JSON tool-call alphabet when linked; else numeric picks.
 * tool_names from cnet_jtc_tool_names if n_labels==CNET_JTC_N_TOOL convention.
 * Returns bytes or -1. */
int cnet_serve_decode_with_jtc(const int *picks, size_t n_picks,
                               char *out, size_t out_cap);

/* Unified presentation: fill text_out from picks + optional labels/jtc. */
int cnet_serve_present(const int *picks, size_t n_picks,
                       const char *const *labels, size_t n_labels,
                       int use_jtc_alphabet,
                       char *text_out, size_t text_cap);

#ifdef __cplusplus
}
#endif
#endif
