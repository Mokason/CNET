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
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

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

#ifndef _WIN32
    {
        const char *read_link = "build/mcp_security_read_link.txt";
        const char *dir_link = "build/mcp_security_dir_link";
        unlink(read_link);
        unlink(dir_link);
        CHECK(symlink("../README.md", read_link) == 0,
              "read-policy symlink fixture created");
        CHECK(mcp_resolve_read_path(read_link, out, sizeof out) != 0,
              "read policy rejects a symlink escape");
        CHECK(symlink("..", dir_link) == 0,
              "write-policy parent-symlink fixture created");
        CHECK(mcp_resolve_write_path(
                  "build/mcp_security_dir_link/escaped.txt", out,
                  sizeof out) != 0,
              "write policy rejects a symlinked parent");
        unlink(read_link);
        unlink(dir_link);
    }
    {
        const char *sentinel = "mcp_security_sentinel.txt";
        const char *target = "build/mcp_security_atomic.txt";
        const char *hostile_tmp = "build/mcp_security_atomic.txt.tmp";
        char contents[64] = {0};
        FILE *f;
        struct stat st;
        remove(target);
        unlink(hostile_tmp);
        f = fopen(sentinel, "wb");
        CHECK(f && fwrite("SENTINEL", 1, 8, f) == 8,
              "atomic-write sentinel fixture created");
        if (f) fclose(f);
        CHECK(symlink("../mcp_security_sentinel.txt", hostile_tmp) == 0,
              "atomic-write hostile temp symlink created");
        CHECK(mcp_atomic_write_text(target, "replacement") == 0,
              "atomic write succeeds despite hostile legacy temp name");
        f = fopen(sentinel, "rb");
        if (f) {
            size_t got = fread(contents, 1, sizeof contents - 1, f);
            contents[got] = '\0';
            fclose(f);
        }
        CHECK(strcmp(contents, "SENTINEL") == 0,
              "atomic write never follows a hostile temp symlink");
        CHECK(lstat(target, &st) == 0 && !S_ISLNK(st.st_mode),
              "atomic write publishes a regular destination");
        remove(target);
        unlink(hostile_tmp);
        remove(sentinel);
    }
#endif
}

static void test_mcp_transport_policy(void) {
    char out[256];
    char long_url[1200];
    FILE *source;
    char source_text[32768];
    size_t got = 0;

    CHECK(mcp_http_get("http://api.duckduckgo.com/", out, sizeof out) != 0,
          "transport blocks cleartext HTTP");
    CHECK(mcp_http_get("https://example.com/", out, sizeof out) != 0,
          "transport blocks arbitrary HTTPS host");
    CHECK(mcp_http_get("https://api.duckduckgo.com.evil.invalid/", out,
                       sizeof out) != 0,
          "transport blocks lookalike allowlist host");
    memset(long_url, 'a', sizeof long_url);
    memcpy(long_url, "https://api.duckduckgo.com/", 27);
    long_url[sizeof long_url - 1] = 0;
    CHECK(mcp_http_get(long_url, out, sizeof out) != 0,
          "transport blocks overlong URL before execution");

    source = fopen("src/contract/mcp_utils.c", "rb");
    if (source) {
        got = fread(source_text, 1, sizeof source_text - 1, source);
        fclose(source);
        source_text[got] = 0;
    }
    CHECK(source != NULL && strstr(source_text, "popen(") == NULL &&
              strstr(source_text, "system(") == NULL,
          "Linux transport contains no shell execution path");
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
    test_mcp_transport_policy();
    test_mcp_memory_roundtrip();
    if (failures == 0) {
        printf("MCP_SECURITY PASS\\n");
        return 0;
    }
    printf("MCP_SECURITY FAIL: %d checks failed.\\n", failures);
    return 1;
}
