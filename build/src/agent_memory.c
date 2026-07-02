/*
 * src/agent_memory.c
 * CNET demo build memory and tool-contract compatibility implementation.
 */

#include "agent_memory.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void format_timestamp(char *out, size_t out_sz) {
    time_t now = time(NULL);
    struct tm *time_ptr = localtime(&now);
    if (time_ptr) {
        struct tm tm_now = *time_ptr;
        strftime(out, out_sz, "%Y-%m-%d %H:%M:%S", &tm_now);
        return;
    }

    (void)snprintf(out, out_sz, "unknown");
}

void sanitize_for_filename(const char *input, char *out, size_t cap) {
    if (!out || cap == 0) {
        return;
    }

    if (!input) {
        out[0] = '\0';
        return;
    }

    size_t n = 0;
    int prev_sep = 0;

    for (size_t i = 0; input[i] != '\0' && n + 1 < cap; ++i) {
        const unsigned char ch = (unsigned char)input[i];
        int allowed = (isalnum(ch) || ch == '_' || ch == '-' || ch == '.');
        if (allowed) {
            out[n++] = (char)ch;
            prev_sep = 0;
        } else if (!prev_sep && isspace(ch)) {
            out[n++] = '_';
            prev_sep = 1;
        } else if (!prev_sep) {
            out[n++] = '_';
            prev_sep = 1;
        }
    }

    while (n > 0 && out[n - 1] == '_') {
        n--;
    }

    if (n == 0) {
        (void)snprintf(out, cap, "output");
        return;
    }

    out[n] = '\0';
}

void agent_record_thought(const char *thought) {
    if (!thought) {
        thought = "(no thought)";
    }

    FILE *log = fopen("agent_thoughts.txt", "a");
    if (!log) {
        return;
    }

    char timestamp[32];
    format_timestamp(timestamp, sizeof(timestamp));
    fprintf(log, "[%s] THOUGHT: %s\n", timestamp, thought);
    fclose(log);
}

int port_contract_mcp_file_write(const char *path, const char *content, char *result_out, size_t cap) {
    if (!path || !content || !result_out || cap == 0) {
        if (result_out && cap > 0) {
            (void)snprintf(result_out, cap, "Failed: invalid arguments.");
        }
        return -1;
    }

    FILE *file = fopen(path, "w");
    if (!file) {
        (void)snprintf(result_out, cap, "Failed: could not open '%s' for write.", path);
        return -1;
    }

    size_t written = fwrite(content, 1, strlen(content), file);
    int fail = ferror(file);
    fclose(file);

    if (fail) {
        (void)snprintf(result_out, cap, "Failed to write artifact '%s'.", path);
        return -1;
    }

    (void)snprintf(result_out, cap, "Wrote %zu bytes to %s", written, path);
    return 0;
}
