#include "../../include/contract/mcp_file_read.h"
#include "../../include/contract/mcp_wiki.h"
#include "../../include/contract/mcp_utils.h"

#include <stdio.h>
#include <string.h>

int port_contract_mcp_file_read(
    const char *path,
    char *content_out, size_t content_cap,
    int *from_cache
) {
    char safe_path[1024];
    FILE *f;
    size_t total = 0;
    char buf[1024];

    if (!path || !content_out || content_cap == 0 || !from_cache) return -1;
    if (mcp_resolve_read_path(path, safe_path, sizeof(safe_path)) != 0) {
        snprintf(content_out, content_cap, "File read blocked by policy: unsafe path");
        return -1;
    }
    *from_cache = 0;

    if (mcp_recall_fact(safe_path, content_out, content_cap)) {
        *from_cache = 1;
        return 0;
    }

    f = fopen(safe_path, "r");
    if (!f) {
        snprintf(content_out, content_cap, "Failed to open file: %s", safe_path);
        return 0;
    }

    content_out[0] = '\0';
    while (fgets(buf, sizeof(buf), f) != NULL && total < content_cap - 1) {
        size_t len = strlen(buf);
        if (len > 0 && total + len >= content_cap - 1) {
            len = content_cap - 1 - total;
            buf[len] = '\0';
        }
        memcpy(content_out + total, buf, len);
        total += len;
        content_out[total] = '\0';
        if (total >= 1500) break;
    }
    fclose(f);

    if (strlen(content_out) < 5) {
        snprintf(content_out, content_cap, "File %s appears empty or unreadable.", safe_path);
    }

    mcp_memorize_fact(safe_path, content_out);
    return 0;
}


