#ifndef CONTRACT_MCP_WEB_SEARCH_H
#define CONTRACT_MCP_WEB_SEARCH_H

#include <stddef.h>

/* MCP Web Search tool.
 * General web search (via DuckDuckGo JSON API for portability).
 * Filters results to top snippets.
 * Integrates with agent memory for caching results.
 * Used for broad knowledge lookup beyond Wikipedia.
 */

int port_contract_mcp_web_search(
    const char *query,
    char *results_out, size_t results_cap,
    int *from_cache
);

#endif /* CONTRACT_MCP_WEB_SEARCH_H */



