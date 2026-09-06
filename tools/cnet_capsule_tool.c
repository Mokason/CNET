/* Independent finite evidence oracle. No model, registry or shell execution. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cnet_capsule_tool_internal.h"
int capsule_tool_integer(const char *s, unsigned *value) {
    if (!s || !*s || strlen(s) > 5) return -1;
    for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return -1;
    errno = 0; char *end;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || *end || n > 65535) return -1;
    *value = (unsigned)n; return 0;
}
int capsule_tool_eval(const char *op, unsigned operand, unsigned x, unsigned *output) {
    uint64_t y;
    if (!strcmp(op, "mul")) y = (uint64_t)x * operand;
    else if (!strcmp(op, "xor")) y = x ^ operand;
    else return 2;
    if (y > 65535) return 2;
    *output = (unsigned)y;
    return 0;
}
int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "plan") || !strcmp(argv[1], "validate"))) return capsule_tool_plan(argc, argv);
    unsigned operand, x, y;
    if (argc != 4 || capsule_tool_integer(argv[2], &operand) ||
        capsule_tool_integer(argv[3], &x) || capsule_tool_eval(argv[1], operand, x, &y)) return 2;
    printf("%u\n", y);
    return 0;
}
