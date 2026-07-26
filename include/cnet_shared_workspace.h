#ifndef CNET_SHARED_WORKSPACE_H
#define CNET_SHARED_WORKSPACE_H

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_WORKSPACE_MAX_ENTRIES 64u
#define CNET_WORKSPACE_SOURCE_MAX 64u
#define CNET_WORKSPACE_TEXT_MAX 256u

typedef enum CnetWorkspaceKind {
    CNET_WORKSPACE_CANDIDATE = 1,
    CNET_WORKSPACE_NOTE = 2,
    CNET_WORKSPACE_EVIDENCE = 3,
    CNET_WORKSPACE_LATENT_REF = 4
} CnetWorkspaceKind;

typedef enum CnetWorkspaceTrust {
    CNET_WORKSPACE_UNCERTIFIED = 0,
    CNET_WORKSPACE_PROVISIONAL = 1,
    CNET_WORKSPACE_CERTIFIED = 2
} CnetWorkspaceTrust;

typedef struct CnetWorkspaceEntry {
    uint64_t sequence;
    uint64_t timestamp_ms;
    CnetWorkspaceKind kind;
    CnetWorkspaceTrust trust;
    double score;
    char source_tag[CNET_WORKSPACE_SOURCE_MAX];
    char text_or_latent_ref[CNET_WORKSPACE_TEXT_MAX];
} CnetWorkspaceEntry;

typedef struct CnetSharedWorkspace {
    CnetWorkspaceEntry entries[CNET_WORKSPACE_MAX_ENTRIES];
    size_t capacity;
    size_t count;
    size_t next;
    uint64_t next_sequence;
} CnetSharedWorkspace;

CNET_API int cnet_workspace_init(CnetSharedWorkspace *workspace, size_t capacity);
CNET_API void cnet_workspace_clear(CnetSharedWorkspace *workspace);
CNET_API int cnet_workspace_push(CnetSharedWorkspace *workspace,
                                 CnetWorkspaceKind kind,
                                 CnetWorkspaceTrust trust,
                                 const char *source_tag,
                                 const char *text_or_latent_ref,
                                 double score,
                                 uint64_t timestamp_ms);
CNET_API size_t cnet_workspace_recent(const CnetSharedWorkspace *workspace,
                                      CnetWorkspaceEntry *out,
                                      size_t out_capacity);
CNET_API size_t cnet_workspace_query(const CnetSharedWorkspace *workspace,
                                     const char *needle,
                                     CnetWorkspaceEntry *out,
                                     size_t out_capacity);
CNET_API uint64_t cnet_workspace_digest(const CnetSharedWorkspace *workspace);

#ifdef __cplusplus
}
#endif

#endif
