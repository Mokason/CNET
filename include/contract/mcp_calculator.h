#ifndef CONTRACT_MCP_CALCULATOR_H
#define CONTRACT_MCP_CALCULATOR_H

#include <stddef.h>

/* MCP Calculator tool (priority 4).
 * Simple arithmetic, delegates to decimal domain if possible.
 * For chaining with memory.
 */

int port_contract_mcp_calculator(
    const char *expr,
    char *result_out, size_t cap,
    int *from_mem
);

#endif /* CONTRACT_MCP_CALCULATOR_H */



