#include "cnet_live_miss.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t n;
    if (!dst || !cap) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int extract_json_str(const char *line, const char *key, char *out,
                            size_t cap) {
    char pat[72];
    const char *p;
    size_t i = 0;
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    p = strstr(line, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0;
    return out[0] ? 0 : -1;
}

int cnet_live_parse_tag_n(const char *turn, char *tag_out, size_t tag_cap,
                          unsigned *n_out) {
    const char *p = turn;
    char tag[CNET_LIVE_DOM_NAME];
    size_t ti = 0;
    unsigned v = 0;
    int saw = 0;
    if (!turn || !n_out) return -1;
    while (*p == ' ' || *p == '\t') p++;
    if (!(isalpha((unsigned char)*p) || *p == '_')) return -1;
    while (*p && *p != ' ' && *p != '\t' && ti + 1 < sizeof tag)
        tag[ti++] = *p++;
    tag[ti] = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    while (isdigit((unsigned char)*p)) {
        saw = 1;
        v = v * 10u + (unsigned)(*p - '0');
        if (v > 15u) return -1;
        p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '\0') return -1; /* teach forms handled elsewhere */
    if (!saw || !tag[0]) return -1;
    if (tag_out && tag_cap) copy_text(tag_out, tag_cap, tag);
    *n_out = v;
    return 0;
}

int cnet_live_parse_teach(const char *turn, char *tag_out, size_t tag_cap,
                          unsigned *in_out, unsigned *out_v) {
    const char *p = turn;
    char tag[CNET_LIVE_DOM_NAME];
    unsigned a = 0, b = 0;
    size_t ti = 0;
    if (!turn || !in_out || !out_v) return -1;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "teach ", 6) == 0) {
        p += 6;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && ti + 1 < sizeof tag) tag[ti++] = *p++;
        tag[ti] = 0;
        if (sscanf(p, " %u %u", &a, &b) != 2) return -1;
    } else if (strncmp(p, "fill ", 5) == 0) {
        if (sscanf(p + 5, "%31s slot=%u out=%u", tag, &a, &b) != 3 &&
            sscanf(p + 5, "%31s %u %u", tag, &a, &b) != 3)
            return -1;
    } else {
        while (*p && *p != ' ' && ti + 1 < sizeof tag) tag[ti++] = *p++;
        tag[ti] = 0;
        if (sscanf(p, " %u = %u", &a, &b) != 2 && sscanf(p, " %u=%u", &a, &b) != 2)
            return -1;
    }
    if (!tag[0] || a > 15 || b > 15) return -1;
    if (tag_out && tag_cap) copy_text(tag_out, tag_cap, tag);
    *in_out = a;
    *out_v = b;
    return 0;
}

int cnet_live_miss_append(const char *miss_path, const char *domain, unsigned in_n,
                          int has_out, unsigned out_n) {
    FILE *f;
    if (!miss_path || !domain || !domain[0] || in_n > 15) return -1;
    if (has_out && out_n > 15) return -1;
    f = fopen(miss_path, "a");
    if (!f) return -1;
    if (has_out) {
        fprintf(f,
                "{\"via\":\"typed_miss\",\"domain\":\"%s\",\"goal_type\":\"%s\","
                "\"in\":%u,\"out\":%u,\"has_in\":1,\"has_out\":1,\"certified\":1,"
                "\"trace_id\":\"tm_%s_%u\"}\n",
                domain, domain, in_n, out_n, domain, in_n);
    } else {
        fprintf(f,
                "{\"via\":\"typed_miss\",\"domain\":\"%s\",\"goal_type\":\"%s\","
                "\"in\":%u,\"has_in\":1,\"has_out\":0,\"certified\":0,"
                "\"trace_id\":\"tm_%s_%u\",\"abstain_reason\":\"need_out\"}\n",
                domain, domain, in_n, domain, in_n);
    }
    fclose(f);
    return 0;
}

int cnet_live_miss_domain_pairs(const char *miss_path, const char *domain,
                                float lut[16]) {
    FILE *f;
    char line[768];
    unsigned seen[16];
    int got = 0;
    if (!miss_path || !domain || !lut) return -1;
    memset(seen, 0, sizeof seen);
    memset(lut, 0, 16 * sizeof(float));
    f = fopen(miss_path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char dbuf[CNET_LIVE_DOM_NAME];
        char *pi, *po;
        unsigned in, out;
        if (extract_json_str(line, "domain", dbuf, sizeof dbuf) != 0 &&
            extract_json_str(line, "goal_type", dbuf, sizeof dbuf) != 0)
            continue;
        if (strcmp(dbuf, domain) != 0) continue;
        if (!strstr(line, "\"out\":")) continue;
        pi = strstr(line, "\"in\":");
        po = strstr(line, "\"out\":");
        if (!pi || !po) continue;
        in = (unsigned)atoi(pi + 5);
        out = (unsigned)atoi(po + 6);
        if (in > 15 || out > 15) continue;
        lut[in] = (float)out;
        if (!seen[in]) {
            seen[in] = 1;
            got++;
        }
    }
    fclose(f);
    return got;
}

int cnet_live_miss_complete_domains(const char *miss_path,
                                    char domains[][CNET_LIVE_DOM_NAME],
                                    int max_dom) {
    FILE *f;
    char line[768];
    char found[64][CNET_LIVE_DOM_NAME];
    int n_found = 0, n_complete = 0, i;
    float lut[16];
    if (!miss_path || !domains || max_dom <= 0) return -1;
    f = fopen(miss_path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f) && n_found < 64) {
        char dbuf[CNET_LIVE_DOM_NAME];
        int exists = 0;
        if (extract_json_str(line, "domain", dbuf, sizeof dbuf) != 0 &&
            extract_json_str(line, "goal_type", dbuf, sizeof dbuf) != 0)
            continue;
        if (!dbuf[0] || strcmp(dbuf, "agi_turn") == 0) continue;
        for (i = 0; i < n_found; ++i)
            if (strcmp(found[i], dbuf) == 0) exists = 1;
        if (!exists) copy_text(found[n_found++], CNET_LIVE_DOM_NAME, dbuf);
    }
    fclose(f);
    for (i = 0; i < n_found && n_complete < max_dom; ++i) {
        if (cnet_live_miss_domain_pairs(miss_path, found[i], lut) == 16) {
            copy_text(domains[n_complete], CNET_LIVE_DOM_NAME, found[i]);
            n_complete++;
        }
    }
    return n_complete;
}
