#ifndef CONTRACT_MCP_UTILS_H
#define CONTRACT_MCP_UTILS_H

#include <stddef.h>

/* Shared MCP safety and transport helpers.
 * Centralized helpers are intentionally conservative:
 *  - policy-aware path validation for MCP file tools
 *  - deterministic key normalization for memory lookups
 *  - bounded command-free URL fetch helper for tool networking
 *  - atomic file write helper to avoid partial writes
 */

void mcp_trim_ws(char *s);
void mcp_sanitize_cache_key(const char *in, char *out, size_t cap);
void mcp_normalize_cache_key(const char *in, char *out, size_t cap);

int mcp_resolve_read_path(const char *in, char *out, size_t cap);
int mcp_resolve_write_path(const char *in, char *out, size_t cap);

int mcp_url_encode_component(const char *in, char *out, size_t cap);
int mcp_http_get(const char *url, char *out, size_t out_cap);
int mcp_extract_json_field(const char *json, const char *key,
                          char *out, size_t cap);

int mcp_atomic_write_text(const char *path, const char *content);

#endif /* CONTRACT_MCP_UTILS_H */




