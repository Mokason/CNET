#ifndef CNET_MCP_READ_BRICK_H
#define CNET_MCP_READ_BRICK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Command families: "wiki search QUERY" and "web read HTTPS_URL".
 * Keywords are ASCII case-insensitive; arguments are preserved byte-for-byte.
 * Keywords require single ASCII spaces. Near-matching control/whitespace
 * separators are terminal refusals, not permission to try a legacy route.
 * Returns 0 only for an unrelated intent. A positive result is TERMINAL,
 * including refusal: callers MUST NOT fall through to another tool/teacher.
 * out receives validated cnet.web-evidence.v1 JSON or a fixed refusal object;
 * truncated evidence is never returned. Even an unusable output buffer leaves
 * an explicit command handled. Retrieved content is untrusted and never CERT.
 * Requires exact CNET_MCP_READ_ENABLED=1, an explicit CNET_MCP_READ_CAPSULES
 * inventory and the certified mcp_read_dispatch_v1 finite mapping. Every call
 * reopens/validates the inventory. No request can select a tool or capsule name.
 * Server-owned HTTPS/host/DNS/budget policy is an additional mandatory gate.
 * Query budget: 200 UTF-16 code units, matching the server; URL: 2048 UTF-8
 * bytes. Malformed UTF-8 and C0/C1 control characters refuse before any I/O.
 */
int cnet_mcp_read_brick_ask(const char *q, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
