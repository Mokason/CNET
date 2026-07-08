#ifndef CONTRACT_MCP_SUMMARIZER_H
#define CONTRACT_MCP_SUMMARIZER_H

#include <stddef.h>

/* MCP Summarizer (priority 4).
 * Basic text summarizer, uses memory.
 */

int port_contract_mcp_summarizer(
    const char *text,
    char *summary_out, size_t cap,
    int *from_mem
);

#endif /* CONTRACT_MCP_SUMMARIZER_H */



