#ifndef CONTRACT_MCP_FILE_READ_H
#define CONTRACT_MCP_FILE_READ_H

#include <stddef.h>
#include "../cnet_export.h"

/* MCP Local File Read tool.
 * Reads a local text file (e.g. book.txt), returns filtered content or summary.
 * Supports agent_ingest_file internally.
 * Caches reads in memory layer.
 */

CNET_API int port_contract_mcp_file_read(
    const char *path,
    char *content_out, size_t content_cap,
    int *from_cache
);

#endif /* CONTRACT_MCP_FILE_READ_H */



