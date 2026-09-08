#include "cnet_mcp_read_brick.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3 || !argv[1][0]) {
        fprintf(stderr, "usage: %s CAPSULE_ROOT 'wiki search QUERY|web read HTTPS_URL'\n"
                "Requires CNET_MCP_READ_ENABLED=1 and a separately enabled, approved MCP peer.\n", argv[0]);
        return 2;
    }
    if (setenv("CNET_MCP_READ_CAPSULES", argv[1], 1)) return 3;
    char *out = calloc(262144, 1);
    if (!out) return 3;
    if (!cnet_mcp_read_brick_ask(argv[2], out, 262144)) {
        puts("{\"status\":\"not_handled\",\"trusted\":false,\"certified\":false}");
        free(out);
        return 2;
    }
    if (!out[0]) { fprintf(stderr, "MCP_READ_REFUSED output_unavailable\n"); free(out); return 3; }
    puts(out);
    int rc = !strncmp(out, "{\"status\":\"refused\"", 19) ? 3 : 0;
    free(out);
    return rc;
}
