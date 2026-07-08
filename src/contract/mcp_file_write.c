#include "../../include/contract/mcp_file_write.h"
#include "../../include/contract/mcp_wiki.h"
#include "../../include/contract/mcp_utils.h"

#include <stdio.h>
#include <string.h>

int port_contract_mcp_file_write(
    const char *path,
    const char *content,
    char *result_out, size_t cap
) {
    char safe_path[1024];

    if (!path || !content || !result_out || cap == 0) return -1;
    if (mcp_resolve_write_path(path, safe_path, sizeof(safe_path)) != 0) {
        snprintf(result_out, cap, "Write blocked by policy: unsafe path");
        return -1;
    }

    if (mcp_atomic_write_text(safe_path, content) != 0) {
        snprintf(result_out, cap, "Failed to write to file: %s", safe_path);
        return 0;
    }

    snprintf(result_out, cap, "Wrote %zu bytes to %s", strlen(content), safe_path);
    mcp_memorize_fact(safe_path, result_out);
    return 0;
}


