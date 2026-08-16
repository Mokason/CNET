#ifndef CONTRACT_MCP_WIKI_H
#define CONTRACT_MCP_WIKI_H

#include <stddef.h>
#include "../cnet_export.h"

/* MCP-style external knowledge contract (v1).
 * First capability: Wikipedia summary lookup.
 * - Looks up via allowlisted HTTPS APIs.
 * - Filters the extract (first N sentences / length).
 * - Uses a separate memory layer (cnet_mcp_facts.bin) for recall before network.
 * - Memorizes successful lookups for reuse across runs.
 *   (Binary-named to keep workspace free of stray runtime .md data files; only README + docs/ kept.)
 *
 * This gives the CNET agent a tool-use + persistent memory slice
 * without polluting the neural chunk / registry layer.
 */

#define MCP_WIKI_MEMORY_FILE "cnet_mcp_facts.bin"

/* Initialize the memory layer (idempotent). Creates file if missing. */
CNET_API int mcp_memory_init(void);

/* Recall a fact. Returns 1 if found (and copied), 0 if miss. */
int mcp_recall_fact(const char *query, char *out_buf, size_t buf_cap);

/* Memorize (or update) a fact. */
void mcp_memorize_fact(const char *query, const char *summary);

/* Main MCP entry: Wikipedia lookup with memory-first + filter.
 * query: natural language or title (e.g. "Alan Turing", "who is Ada Lovelace")
 * summary_out: receives filtered summary
 * from_memory: set to 1 if served from the separate memory layer
 * Returns 0 on success (even if "LOOKUP_FAILED" content), -1 on bad args.
 */
int port_contract_mcp_wiki_lookup(
    const char *query,
    char *summary_out,
    size_t summary_out_cap,
    int *from_memory
);

#endif /* CONTRACT_MCP_WIKI_H */




