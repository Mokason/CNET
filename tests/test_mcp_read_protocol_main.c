#include "cnet_mcp_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    char *out = calloc(262144, 1);
    if (!out || argc != 3) { free(out); return 2; }
    if (!strcmp(argv[1], "--summary")) {
        int n = cnet_mcp_read_summary(argv[2], out, 262144);
        if (n) puts(out);
        free(out);
        return n ? 0 : 1;
    }
    int n = cnet_mcp_read_call(argv[1], argv[2], out, 262144);
    if (n) puts(out);
    free(out);
    return n ? 0 : 1;
}
