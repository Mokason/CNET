#include "../../include/contract/mcp_calculator.h"
#include "../../include/contract/mcp_wiki.h"
#include "../../include/agent_memory.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int port_contract_mcp_calculator(
    const char *expr,
    char *result_out, size_t cap,
    int *from_mem
) {
    if (!expr || !result_out || cap == 0 || !from_mem) return -1;
    *from_mem = 0;

    char key[128];
    snprintf(key, sizeof(key), "calc:%s", expr);
    if (mcp_recall_fact(key, result_out, cap)) {
        *from_mem = 1;
        return 0;
    }

    /* Simple eval for + - * / on numbers */
    double a=0, b=0; char op;
    if (sscanf(expr, "%lf %c %lf", &a, &op, &b) == 3) {
        double res = 0;
        if (op == '+') res = a+b;
        else if (op == '-') res = a-b;
        else if (op == '*') res = a*b;
        else if (op == '/') {
            if (b == 0.0) {
                snprintf(result_out, cap, "Division by zero");
            } else {
                snprintf(result_out, cap, "%.2f", a / b);
            }
            mcp_memorize_fact(key, result_out);
            return 0;
        }
        snprintf(result_out, cap, "%.2f", res);
    } else {
        snprintf(result_out, cap, "Could not parse expr: %s", expr);
    }

    mcp_memorize_fact(key, result_out);
    return 0;
}


