#include "cnet_chat_lookup.h"

#include <ctype.h>
#include <string.h>

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t n;
    if (dst == NULL || cap == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int scheme_prefix(const char *text, size_t *len_out) {
    static const char *const schemes[] = {
        "https://", "http://", "file://", "javascript:", "mailto:", "ftp://",
        "data:", NULL
    };
    size_t i;
    if (text == NULL) return 0;
    for (i = 0; schemes[i] != NULL; ++i) {
        size_t n = strlen(schemes[i]);
        if (strncmp(text, schemes[i], n) == 0) {
            if (len_out != NULL) *len_out = n;
            return 1;
        }
    }
    return 0;
}

static int url_stop(char c) {
    return c == '\0' || isspace((unsigned char)c) || c == '>' || c == ')' ||
           c == '"' || c == '\'' || c == '<' || c == ']';
}

int cnet_chat_lookup_offer_url(const char *turn, char *url, size_t cap) {
    const char *cursor, *end;
    size_t n;
    if (url == NULL || cap == 0) return 0;
    url[0] = '\0';
    if (turn == NULL || turn[0] == '\0') return 0;
    for (cursor = turn; *cursor != '\0'; ++cursor) {
        if (!scheme_prefix(cursor, NULL)) continue;
        end = cursor;
        while (!url_stop(*end)) ++end;
        while (end > cursor &&
               (end[-1] == '.' || end[-1] == ',' || end[-1] == ';' ||
                end[-1] == '?' || end[-1] == '!'))
            --end;
        n = (size_t)(end - cursor);
        if (n == 0) continue;
        if (n >= cap) n = cap - 1u;
        memcpy(url, cursor, n);
        url[n] = '\0';
        return 1;
    }
    return 0;
}

static int has_word_ci(const char *text, const char *word) {
    const char *cursor;
    size_t n, i;
    if (text == NULL || word == NULL || word[0] == '\0') return 0;
    n = strlen(word);
    for (cursor = text; *cursor != '\0'; ++cursor) {
        int left, right, match = 1;
        if (cursor > text && isalnum((unsigned char)cursor[-1])) continue;
        for (i = 0; i < n; ++i) {
            if (tolower((unsigned char)cursor[i]) !=
                tolower((unsigned char)word[i])) {
                match = 0;
                break;
            }
        }
        if (!match) continue;
        right = !isalnum((unsigned char)cursor[n]);
        left = (cursor == text) || !isalnum((unsigned char)cursor[-1]);
        if (left && right) return 1;
    }
    return 0;
}

CnetLookupBind cnet_chat_lookup_infer_bind(const char *turn) {
    if (has_word_ci(turn, "year")) return CNET_LOOKUP_BIND_YEAR;
    if (has_word_ci(turn, "token")) return CNET_LOOKUP_BIND_TOKEN;
    if (has_word_ci(turn, "line")) return CNET_LOOKUP_BIND_LINE;
    if (has_word_ci(turn, "integer") || has_word_ci(turn, "number"))
        return CNET_LOOKUP_BIND_INTEGER;
    return CNET_LOOKUP_BIND_INTEGER;
}

int cnet_chat_lookup_turn_flags(const char *turn, unsigned flags,
                                CnetChatLookupTurn *out) {
    if (out == NULL) return -1;
    memset(out, 0, sizeof *out);
    /* residual_calls stays 0: this path never calls utter residual. */
    out->bind = cnet_chat_lookup_infer_bind(turn);
    out->offered = cnet_chat_lookup_offer_url(turn, out->url, sizeof out->url);
    if (!out->offered) {
        copy_text(out->refusal, sizeof out->refusal, "empty_url");
        return 1;
    }
    {
        int rc = cnet_lookup_execute_flags(out->url, out->bind, flags,
                                           &out->report);
        copy_text(out->refusal, sizeof out->refusal, out->report.refusal);
        if (rc != 0) return rc == 1 ? 1 : rc;
        if (!out->report.bound) {
            if (out->refusal[0] == '\0')
                copy_text(out->refusal, sizeof out->refusal, "unbindable");
            return 1;
        }
        if (cnet_lookup_speak(&out->report, out->spoken, sizeof out->spoken) !=
            0) {
            out->spoken[0] = '\0';
            copy_text(out->refusal, sizeof out->refusal, "unbindable");
            return 1;
        }
        out->answered = 1;
        return 0;
    }
}

int cnet_chat_lookup_turn(const char *turn, CnetChatLookupTurn *out) {
    return cnet_chat_lookup_turn_flags(turn, 0, out);
}

int cnet_chat_lookup_cnetd_hop(const char *turn, CnetChatLookupTurn *out) {
    return cnet_chat_lookup_turn(turn, out);
}
