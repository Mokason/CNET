/*
 * Focused MCP security coverage:
 *  - shared path policy for file tools
 *  - memory layer key/input handling and persistence robustness
 */
#include "../include/contract/mcp_utils.h"
#include "../include/contract/mcp_wiki.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if ((cond)) { printf("  ok   %s\n", (msg)); } \
    else { printf("  FAIL %s\n", (msg)); ++failures; } \
} while (0)

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int copy_file(const char *src, const char *dst) {
    FILE *in;
    FILE *out;
    int c;

    in = fopen(src, "rb");
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((c = fgetc(in)) != EOF) {
        fputc(c, out);
    }
    fclose(in);
    fclose(out);
    return 0;
}

static void test_mcp_path_policy(void) {
    char out[256];

    CHECK(mcp_resolve_read_path("mcp_notes.md", out, sizeof(out)) == 0, "read path allows top-level .md file");
    CHECK(mcp_resolve_read_path("build/report.md", out, sizeof(out)) == 0, "read path allows build path file");
    CHECK(mcp_resolve_read_path("../secret.md", out, sizeof(out)) != 0, "read path blocks directory traversal");
    CHECK(mcp_resolve_read_path("build/../../secret.md", out, sizeof(out)) != 0, "read path blocks nested traversal");
    CHECK(mcp_resolve_read_path("bad|name.md", out, sizeof(out)) != 0, "read path blocks disallowed chars");
    CHECK(mcp_resolve_read_path("report.exe", out, sizeof(out)) != 0, "read path blocks unsupported extension");

    CHECK(mcp_resolve_write_path("build/out.txt", out, sizeof(out)) == 0, "write path allows build path file");
    CHECK(mcp_resolve_write_path("report.txt", out, sizeof(out)) != 0, "write path blocks non-build path");
    CHECK(mcp_resolve_write_path("build/../out.txt", out, sizeof(out)) != 0, "write path blocks traversal");
}

static void test_mcp_memory_roundtrip(void) {
    const char *backup_path = "cnet_mcp_facts.bin._test_backup";
    const char *tmp_path = "cnet_mcp_facts.bin.tmp";
    char out[256];
    int had_original_file = file_exists(MCP_WIKI_MEMORY_FILE);
    int backup_restored = 0;

    if (had_original_file) {
        if (copy_file(MCP_WIKI_MEMORY_FILE, backup_path) != 0) {
            CHECK(0, "existing cnet_mcp_facts.bin backup snapshot captured");
            return;
        }
    }
    remove(MCP_WIKI_MEMORY_FILE);
    remove(tmp_path);

    mcp_memorize_fact("Who is Ada Lovelace", "Pioneer of computing and first programmer.");
    CHECK(mcp_recall_fact("ada lovelace", out, sizeof(out)) != 0, "memory recall works from normalized key");
    CHECK(strcmp(out, "Pioneer of computing and first programmer.") == 0, "memory stores full summary text");

    mcp_memorize_fact("danger|key:test", "one|two|three");
    CHECK(mcp_recall_fact("danger key test", out, sizeof(out)) != 0, "memory recall survives key normalization");
    CHECK(strstr(out, "|") == NULL, "summary sanitizer removes delimiter characters");

    if (mcp_recall_fact("does-not-exist", out, sizeof(out)) == 0) {
        CHECK(1, "memory miss returns no fact");
    } else {
        CHECK(0, "memory miss returns no fact");
    }

    CHECK(file_exists(MCP_WIKI_MEMORY_FILE), "memory file exists after updates");
    CHECK(!file_exists(tmp_path), "atomic temp write file is not left behind");

    if (had_original_file) {
        backup_restored = (copy_file(backup_path, MCP_WIKI_MEMORY_FILE) == 0);
        CHECK(backup_restored, "memory file restored to pre-test snapshot");
    } else {
        remove(MCP_WIKI_MEMORY_FILE);
        CHECK(!file_exists(MCP_WIKI_MEMORY_FILE), "temporary test memory file removed");
    }
    if (backup_restored || !had_original_file) {
        remove(backup_path);
    }
}

int run_test_mcp_security(void) {
    printf("\\n-- MCP security and persistence checks --\\n");
    test_mcp_path_policy();
    test_mcp_memory_roundtrip();
    if (failures == 0) {
        printf("MCP_SECURITY PASS\\n");
        return 0;
    }
    printf("MCP_SECURITY FAIL: %d checks failed.\\n", failures);
    return 1;
}
