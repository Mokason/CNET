#ifndef CONTRACT_MCP_FILE_WRITE_H
#define CONTRACT_MCP_FILE_WRITE_H

#include <stddef.h>

/* MCP File Write tool for "build" outputs.
 * Writes content to a file (e.g. to build artifacts, stories, knowledge exports).
 * Integrates with memory for "built" items.
 */

int port_contract_mcp_file_write(
    const char *path,
    const char *content,
    char *result_out, size_t cap
);

#endif /* CONTRACT_MCP_FILE_WRITE_H */



