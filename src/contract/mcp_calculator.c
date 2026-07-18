#include "../../include/contract/mcp_calculator.h"
#include "../../include/contract/mcp_math_eval.h"
#include "../../include/contract/mcp_wiki.h"

#include <stdio.h>
#include <string.h>

/* Phase 1: calculator is a thin MCP face over math_eval (richer expressions).
 * Legacy "a OP b" still works; also accepts sqrt(), ^, parentheses, etc.
 */
int port_contract_mcp_calculator(
    const char *expr,
    char *result_out, size_t cap,
    int *from_mem)
{
    int rc;
    if (!expr || !result_out || cap == 0 || !from_mem) return -1;
    rc = port_contract_mcp_math_eval(expr, result_out, cap, from_mem);
    if (rc != 0) return rc;
    /* Map eval errors to legacy-ish messages for callers */
    if (strcmp(result_out, "EVAL_ERROR") == 0) {
        snprintf(result_out, cap, "Could not parse expr: %s", expr);
    } else if (strcmp(result_out, "Division by zero") == 0) {
        /* already handled in parser as EVAL_ERROR */
    }
    return 0;
}
