#include "cnet_chat1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exact-prompt overlap audit. Prints counts only, never prompt text. */

static int load_prompts(const char *path, char ***out, size_t *count) {
    FILE *file;
    char line[2048];
    char **rows = NULL;
    size_t n = 0, cap = 0, line_no = 0;
    file = fopen(path, "rb");
    if (file == NULL) return -1;
    while (fgets(line, sizeof line, file) != NULL) {
        char *copy, *fields[8];
        size_t i, f = 0;
        char *cursor = line;
        ++line_no;
        if (line_no <= 2) continue;
        while (f < 7) {
            fields[f++] = cursor;
            cursor = strchr(cursor, '\t');
            if (cursor == NULL) break;
            *cursor++ = '\0';
        }
        if (f < 6) {
            fclose(file);
            return -1;
        }
        {
            size_t len = strlen(fields[5]);
            while (len > 0 &&
                   (fields[5][len - 1u] == '\n' || fields[5][len - 1u] == '\r'))
                fields[5][--len] = '\0';
        }
        if (n == cap) {
            char **grown;
            cap = cap ? cap * 2u : 64u;
            grown = (char **)realloc(rows, cap * sizeof *rows);
            if (grown == NULL) {
                fclose(file);
                return -1;
            }
            rows = grown;
        }
        copy = (char *)malloc(strlen(fields[5]) + 1u);
        if (copy == NULL) {
            fclose(file);
            return -1;
        }
        memcpy(copy, fields[5], strlen(fields[5]) + 1u);
        rows[n++] = copy;
        (void)i;
    }
    fclose(file);
    *out = rows;
    *count = n;
    return 0;
}

int main(int argc, char **argv) {
    char **chat = NULL, **prior = NULL;
    size_t chat_n = 0, prior_n = 0, i, j, overlap = 0, dup = 0;
    int rc = 1;
    if (argc != 3) {
        fprintf(stderr, "usage: %s CHAT1.tsv PRIOR.tsv\n", argv[0]);
        return 2;
    }
    if (load_prompts(argv[1], &chat, &chat_n) != 0 ||
        load_prompts(argv[2], &prior, &prior_n) != 0)
        goto done;
    for (i = 0; i < chat_n; ++i)
        for (j = i + 1u; j < chat_n; ++j)
            if (strcmp(chat[i], chat[j]) == 0) ++dup;
    for (i = 0; i < chat_n; ++i)
        for (j = 0; j < prior_n; ++j)
            if (strcmp(chat[i], prior[j]) == 0) ++overlap;
    if (dup != 0 || overlap != 0 || chat_n != CNET_CHAT1_TOTAL_ROWS) {
        printf("CNET_CHAT1_INDEPENDENCE_FAIL rows=%zu dup=%zu overlap=%zu\n",
               chat_n, dup, overlap);
        goto done;
    }
    printf("CNET_CHAT1_INDEPENDENCE_PASS rows=%zu prior=%zu overlap=0 "
           "duplicates=0\n",
           chat_n, prior_n);
    rc = 0;
done:
    for (i = 0; i < chat_n; ++i) free(chat[i]);
    for (i = 0; i < prior_n; ++i) free(prior[i]);
    free(chat);
    free(prior);
    return rc;
}
