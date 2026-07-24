#include "../include/cnet_fault.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void json_escape(const char *in, char *out, size_t cap) {
    size_t j = 0;
    if (!in) in = "";
    for (; *in && j + 2 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            if (j + 3 >= cap) break;
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c < 0x20) {
            continue;
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = 0;
}

const char *cnet_fault_source_name(CnetFaultSource s) {
    switch (s) {
    case CNET_FAULT_SRC_GHOST: return "ghost";
    case CNET_FAULT_SRC_JTC: return "jtc";
    case CNET_FAULT_SRC_MCP: return "mcp";
    case CNET_FAULT_SRC_SALON: return "salon";
    case CNET_FAULT_SRC_SURPRISE: return "surprise";
    case CNET_FAULT_SRC_SYNTH: return "synth";
    default: return "unknown";
    }
}

CnetFaultSource cnet_fault_source_parse(const char *s) {
    if (!s) return CNET_FAULT_SRC_UNKNOWN;
    if (!strcmp(s, "ghost")) return CNET_FAULT_SRC_GHOST;
    if (!strcmp(s, "jtc")) return CNET_FAULT_SRC_JTC;
    if (!strcmp(s, "mcp")) return CNET_FAULT_SRC_MCP;
    if (!strcmp(s, "salon")) return CNET_FAULT_SRC_SALON;
    if (!strcmp(s, "surprise")) return CNET_FAULT_SRC_SURPRISE;
    if (!strcmp(s, "synth")) return CNET_FAULT_SRC_SYNTH;
    return CNET_FAULT_SRC_UNKNOWN;
}

int cnet_fault_open(CnetFaultLog *log, const char *path) {
    const char *p;
    if (!log) return -1;
    memset(log, 0, sizeof *log);
    p = path;
    if (!p || !p[0]) p = getenv("CNET_FAULT_LOG");
    if (!p || !p[0]) p = "cnet_faults.jsonl";
    if (strlen(p) >= sizeof log->path) return -1;
    memcpy(log->path, p, strlen(p) + 1);
    log->fp = fopen(log->path, "a");
    if (!log->fp) return -1;
    return 0;
}

void cnet_fault_close(CnetFaultLog *log) {
    if (!log) return;
    if (log->fp) {
        fclose(log->fp);
        log->fp = NULL;
    }
}

int cnet_fault_append(CnetFaultLog *log, const CnetFaultRecord *rec) {
    char u[128], sk[128], se[96], lk[48], no[200];
    long long ts;
    if (!log || !log->fp || !rec) return -1;
    ts = rec->ts_unix;
    if (ts <= 0) ts = (long long)time(NULL);
    json_escape(rec->unit, u, sizeof u);
    json_escape(rec->skill, sk, sizeof sk);
    json_escape(rec->session, se, sizeof se);
    json_escape(rec->label_kind, lk, sizeof lk);
    json_escape(rec->note, no, sizeof no);
    if (fprintf(log->fp,
                "{\"ts\":%lld,\"source\":\"%s\",\"unit\":\"%s\",\"skill\":\"%s\","
                "\"session\":\"%s\",\"in_dim\":%d,\"out_dim\":%d,"
                "\"label_kind\":\"%s\",\"note\":\"%s\"}\n",
                ts, cnet_fault_source_name(rec->source), u, sk, se,
                rec->in_dim, rec->out_dim, lk[0] ? lk : "argmax", no) < 0)
        return -1;
    fflush(log->fp);
    log->append_count++;
    return 0;
}

size_t cnet_fault_count_file(const char *path) {
    FILE *f;
    char buf[1024];
    size_t n = 0;
    if (!path || !path[0]) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(buf, sizeof buf, f)) {
        if (buf[0] == '{') n++;
    }
    fclose(f);
    return n;
}

/* Minimal field extract: "key":"value" or "key":number */
static int extract_str(const char *line, const char *key, char *out, size_t cap) {
    char pat[80];
    const char *p;
    size_t i = 0;
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    p = strstr(line, pat);
    if (!p) {
        out[0] = 0;
        return 0;
    }
    p += strlen(pat);
    for (; *p && *p != '"' && i + 1 < cap; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            out[i++] = *p;
        } else {
            out[i++] = *p;
        }
    }
    out[i] = 0;
    return 1;
}

static int extract_int(const char *line, const char *key, int *out) {
    char pat[80];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    *out = (int)strtol(p, NULL, 10);
    return 1;
}

static int extract_ll(const char *line, const char *key, long long *out) {
    char pat[80];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\":", key);
    p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    *out = strtoll(p, NULL, 10);
    return 1;
}

size_t cnet_fault_load(const char *path, const char *unit_filter,
                       CnetFaultRecord *out, size_t cap) {
    FILE *f;
    char line[1024];
    size_t n = 0;
    if (!path || !out || !cap) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f) && n < cap) {
        CnetFaultRecord r;
        char src[32];
        memset(&r, 0, sizeof r);
        if (line[0] != '{') continue;
        extract_ll(line, "ts", &r.ts_unix);
        extract_str(line, "source", src, sizeof src);
        r.source = cnet_fault_source_parse(src);
        extract_str(line, "unit", r.unit, sizeof r.unit);
        extract_str(line, "skill", r.skill, sizeof r.skill);
        extract_str(line, "session", r.session, sizeof r.session);
        extract_int(line, "in_dim", &r.in_dim);
        extract_int(line, "out_dim", &r.out_dim);
        extract_str(line, "label_kind", r.label_kind, sizeof r.label_kind);
        extract_str(line, "note", r.note, sizeof r.note);
        if (unit_filter && unit_filter[0] && strcmp(r.unit, unit_filter) != 0)
            continue;
        out[n++] = r;
    }
    fclose(f);
    return n;
}
