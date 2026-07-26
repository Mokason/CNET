#include "cnet_shared_workspace.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

static int workspace_kind_valid(CnetWorkspaceKind kind) {
    return kind >= CNET_WORKSPACE_CANDIDATE &&
           kind <= CNET_WORKSPACE_LATENT_REF;
}

static int workspace_trust_valid(CnetWorkspaceTrust trust) {
    return trust >= CNET_WORKSPACE_UNCERTIFIED &&
           trust <= CNET_WORKSPACE_CERTIFIED;
}

static void workspace_copy(char *dst, size_t dst_size, const char *src) {
    size_t n;
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static size_t workspace_oldest(const CnetSharedWorkspace *workspace) {
    if (workspace->count < workspace->capacity) return 0;
    return workspace->next;
}

static int workspace_valid(const CnetSharedWorkspace *workspace) {
    return workspace && workspace->capacity > 0 &&
           workspace->capacity <= CNET_WORKSPACE_MAX_ENTRIES &&
           workspace->count <= workspace->capacity &&
           workspace->next < workspace->capacity;
}

static const CnetWorkspaceEntry *workspace_at(
    const CnetSharedWorkspace *workspace, size_t chronological_index) {
    const size_t oldest = workspace_oldest(workspace);
    return &workspace->entries[
        (oldest + chronological_index) % workspace->capacity];
}

static int workspace_contains_ci(const char *haystack, const char *needle) {
    size_t i, j, needle_len;
    if (!haystack || !needle || needle[0] == '\0') return 0;
    needle_len = strlen(needle);
    for (i = 0; haystack[i] != '\0'; i++) {
        for (j = 0; j < needle_len && haystack[i + j] != '\0'; j++) {
            const unsigned char h = (unsigned char)haystack[i + j];
            const unsigned char n = (unsigned char)needle[j];
            if (tolower(h) != tolower(n)) break;
        }
        if (j == needle_len) return 1;
    }
    return 0;
}

static uint64_t workspace_hash_bytes(uint64_t hash,
                                     const void *data,
                                     size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t i;
    for (i = 0; i < size; i++) {
        hash ^= (uint64_t)bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

int cnet_workspace_init(CnetSharedWorkspace *workspace, size_t capacity) {
    if (!workspace || capacity == 0 ||
        capacity > CNET_WORKSPACE_MAX_ENTRIES) {
        return -1;
    }
    memset(workspace, 0, sizeof(*workspace));
    workspace->capacity = capacity;
    workspace->next_sequence = 1;
    return 0;
}

void cnet_workspace_clear(CnetSharedWorkspace *workspace) {
    size_t capacity;
    if (!workspace) return;
    capacity = workspace->capacity;
    memset(workspace, 0, sizeof(*workspace));
    workspace->capacity = capacity;
    workspace->next_sequence = 1;
}

int cnet_workspace_push(CnetSharedWorkspace *workspace,
                        CnetWorkspaceKind kind,
                        CnetWorkspaceTrust trust,
                        const char *source_tag,
                        const char *text_or_latent_ref,
                        double score,
                        uint64_t timestamp_ms) {
    CnetWorkspaceEntry *entry;
    if (!workspace_valid(workspace) ||
        !workspace_kind_valid(kind) || !workspace_trust_valid(trust) ||
        !source_tag || source_tag[0] == '\0' ||
        !text_or_latent_ref || text_or_latent_ref[0] == '\0' ||
        !isfinite(score)) {
        return -1;
    }
    entry = &workspace->entries[workspace->next];
    memset(entry, 0, sizeof(*entry));
    entry->sequence = workspace->next_sequence++;
    entry->timestamp_ms = timestamp_ms;
    entry->kind = kind;
    entry->trust = trust;
    entry->score = score;
    workspace_copy(entry->source_tag, sizeof(entry->source_tag), source_tag);
    workspace_copy(entry->text_or_latent_ref,
                   sizeof(entry->text_or_latent_ref),
                   text_or_latent_ref);
    workspace->next = (workspace->next + 1) % workspace->capacity;
    if (workspace->count < workspace->capacity) workspace->count++;
    return 0;
}

size_t cnet_workspace_recent(const CnetSharedWorkspace *workspace,
                             CnetWorkspaceEntry *out,
                             size_t out_capacity) {
    size_t written = 0;
    if (!workspace_valid(workspace) || !out || out_capacity == 0) {
        return 0;
    }
    while (written < workspace->count && written < out_capacity) {
        const size_t chronological = workspace->count - 1 - written;
        out[written] = *workspace_at(workspace, chronological);
        written++;
    }
    return written;
}

size_t cnet_workspace_query(const CnetSharedWorkspace *workspace,
                            const char *needle,
                            CnetWorkspaceEntry *out,
                            size_t out_capacity) {
    size_t i, written = 0;
    if (!workspace_valid(workspace) || !needle || needle[0] == '\0' ||
        !out || out_capacity == 0) {
        return 0;
    }
    for (i = workspace->count; i > 0 && written < out_capacity; i--) {
        const CnetWorkspaceEntry *entry = workspace_at(workspace, i - 1);
        if (workspace_contains_ci(entry->source_tag, needle) ||
            workspace_contains_ci(entry->text_or_latent_ref, needle)) {
            out[written++] = *entry;
        }
    }
    return written;
}

uint64_t cnet_workspace_digest(const CnetSharedWorkspace *workspace) {
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t i;
    if (!workspace_valid(workspace)) return 0;
    for (i = 0; i < workspace->count; i++) {
        const CnetWorkspaceEntry *entry = workspace_at(workspace, i);
        hash = workspace_hash_bytes(hash, &entry->sequence,
                                    sizeof(entry->sequence));
        hash = workspace_hash_bytes(hash, &entry->timestamp_ms,
                                    sizeof(entry->timestamp_ms));
        hash = workspace_hash_bytes(hash, &entry->kind, sizeof(entry->kind));
        hash = workspace_hash_bytes(hash, &entry->trust, sizeof(entry->trust));
        hash = workspace_hash_bytes(hash, &entry->score, sizeof(entry->score));
        hash = workspace_hash_bytes(hash, entry->source_tag,
                                    strlen(entry->source_tag) + 1);
        hash = workspace_hash_bytes(hash, entry->text_or_latent_ref,
                                    strlen(entry->text_or_latent_ref) + 1);
    }
    return hash;
}
