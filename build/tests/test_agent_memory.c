/*
 * tests/test_agent_memory.c
 * Light smoke checks for filename sanitization and file-write behavior.
 */

#include <stdio.h>
#include <string.h>

#include "agent_memory.h"

static int fail_count = 0;

static void check_eq(const char *name, const char *got, const char *expected) {
    if (strcmp(got, expected) != 0) {
        printf("FAIL: %s (got=%s, expected=%s)\n", name, got, expected);
        fail_count++;
    }
}

static void test_sanitize() {
    char buf[64];
    sanitize_for_filename("a build request: Granny + woodchipper", buf, sizeof(buf));
    check_eq("sanitize_punctuation", buf, "a_build_request_Granny_woodchipper");

    sanitize_for_filename("   ", buf, sizeof(buf));
    check_eq("sanitize_whitespace_only", buf, "output");

    sanitize_for_filename("abc___", buf, sizeof(buf));
    check_eq("sanitize_trailing", buf, "abc");
}

static void test_write() {
    char result[128];
    const char *content = "test";
    int rc = port_contract_mcp_file_write("build_memory_test_output.txt", content, result, sizeof(result));
    if (rc != 0) {
        printf("FAIL: write_exit_code (%s)\n", result);
        fail_count++;
    } else {
        printf("PASS: write_exit_code (%s)\n", result);
    }
}

int main(void) {
    test_sanitize();
    test_write();
    if (fail_count != 0) {
        printf("agent_memory tests FAILED: %d\n", fail_count);
        return 1;
    }
    printf("agent_memory tests PASSED\n");
    return 0;
}
