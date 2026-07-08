#include "../../include/contract/mcp_summarizer.h"
#include "../../include/contract/mcp_wiki.h"
#include "../../include/agent_memory.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int port_contract_mcp_summarizer(
    const char *text,
    char *summary_out, size_t cap,
    int *from_mem
) {
    if (!text || !summary_out || cap == 0 || !from_mem) return -1;
    *from_mem = 0;

    char key[128];
    snprintf(key, sizeof(key), "sum:%.*s", 50, text);
    if (mcp_recall_fact(key, summary_out, cap)) {
        *from_mem = 1;
        return 0;
    }

    /* Simple: first 100 chars + last sentence */
    int len = strlen(text);
    if (len < 100) {
        size_t n = (size_t)(len < (int)(cap - 1) ? len : (int)(cap - 1));
        memcpy(summary_out, text, n);
        summary_out[n] = '\0';
    } else {
        size_t n = (cap > 80) ? 80 : cap - 1;
        memcpy(summary_out, text, n);
        summary_out[n] = '\0';
        if (cap > 80) {
            strncat(summary_out, "... [summarized]", cap - strlen(summary_out) -1);
        }
    }

    mcp_memorize_fact(key, summary_out);
    return 0;
}


