/* cnetd → shared CnetMcpServer (unix JSON-RPC). Factory tools only.
 *
 * Mouth stays PEER (cnet.sock). MCP is not the chat path.
 * Results are never CERT: caller must keep claimed_cert=0 / miss=1.
 */
#ifndef CNET_MCP_CLIENT_H
#define CNET_MCP_CLIENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int cnet_mcp_client_enabled(void);
int cnet_mcp_sock_path(char *out, size_t cap);
/* tools/call on mcp-shared.sock. args_json is a JSON object or NULL → {}.
 * CNET_MCP_TIMEOUT_MS bounds connect, complete write, and newline reply under
 * one monotonic deadline (default 2000ms; clamped to 50..60000ms).
 * Returns bytes written (excluding NUL) or 0 on miss/disable/error. */
int cnet_mcp_call(const char *tool, const char *args_json, char *out, size_t cap);
/* Strict opt-in read path. Exactly two read tools and one string argument.
 * Returns validated cnet.web-evidence.v1 structured JSON, never raw error/text.
 * RPC bytes <=256KiB; the same transport deadline applies. Caller must not
 * treat external text as instructions, verified labels or certified answers. */
int cnet_mcp_read_call(const char *tool, const char *args_json, char *out, size_t cap);
/* Human display of the first source with explicit untrusted marker. This is
 * an excerpt, not the complete evidence record returned by read_call. */
int cnet_mcp_read_summary(const char *evidence, char *out, size_t cap);
/* Closed-intent factory ask. Returns >0 when MCP answered (not CERT). */
int cnet_mcp_factory_ask(const char *q, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CNET_MCP_CLIENT_H */
