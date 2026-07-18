#ifndef CONTRACT_MCP_MATH_EVAL_H
#define CONTRACT_MCP_MATH_EVAL_H

#include <stddef.h>
#include "../cnet_export.h"

/* Expression evaluator for creative math plans (Phase 1).
 * Supports: + - * / ^ , unary -, parentheses,
 *           sqrt(), abs(), floor(), ceil(),
 *           and constants pi, e.
 * Returns 0 on success and writes a decimal string; -1 on parse/domain error.
 */

CNET_API int cnet_math_eval_expr(const char *expr, double *out_value);

/* MCP-style wrapper (memory-cached under math_eval:key). */
CNET_API int port_contract_mcp_math_eval(
    const char *expr,
    char *result_out, size_t cap,
    int *from_mem);

#endif /* CONTRACT_MCP_MATH_EVAL_H */
