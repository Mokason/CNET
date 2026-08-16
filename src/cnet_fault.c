#include "../include/cnet_fault.h"
#include "../include/cnet_json_escape.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CNET_FAULT_MAX_DIM 512
#define CNET_FAULT_LINE_MAX (256 * 1024)


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

static int write_meta_fields(FILE *fp, const CnetFaultRecord *rec, long long ts) {
    char u[128], sk[128], se[96], lk[48], no[200];
    /* The previous local escaper returned void, DROPPED every control
       character, and truncated silently -- so a note containing a stray 0x01
       was recorded as if it had never held one. Count losses instead and put
       the count in the record: a field that would not fit comes out empty, and
       `esc_truncated` says so, so the learning loop can tell a genuinely empty
       note from one that was thrown away. */
    int esc_trunc = 0;
    esc_trunc += (cnet_json_escape(rec->unit, u, sizeof u) != 0);
    esc_trunc += (cnet_json_escape(rec->skill, sk, sizeof sk) != 0);
    esc_trunc += (cnet_json_escape(rec->session, se, sizeof se) != 0);
    esc_trunc += (cnet_json_escape(rec->label_kind, lk, sizeof lk) != 0);
    esc_trunc += (cnet_json_escape(rec->note, no, sizeof no) != 0);
    return fprintf(fp,
                   "{\"ts\":%lld,\"source\":\"%s\",\"unit\":\"%s\",\"skill\":\"%s\","
                   "\"session\":\"%s\",\"in_dim\":%d,\"out_dim\":%d,"
                   "\"label_kind\":\"%s\",\"note\":\"%s\",\"esc_truncated\":%d",
                   ts, cnet_fault_source_name(rec->source), u, sk, se,
                   rec->in_dim, rec->out_dim, lk[0] ? lk : "argmax", no, esc_trunc);
}

static int write_vec(FILE *fp, const char *key, const double *v, int n) {
    int i;
    if (fprintf(fp, ",\"%s\":[", key) < 0) return -1;
    for (i = 0; i < n; i++) {
        if (fprintf(fp, "%s%.9g", i ? "," : "", v[i]) < 0) return -1;
    }
    if (fputc(']', fp) == EOF) return -1;
    return 0;
}

int cnet_fault_append(CnetFaultLog *log, const CnetFaultRecord *rec) {
    long long ts;
    if (!log || !log->fp || !rec) return -1;
    ts = rec->ts_unix;
    if (ts <= 0) ts = (long long)time(NULL);
    if (write_meta_fields(log->fp, rec, ts) < 0) return -1;
    if (fputs("}\n", log->fp) < 0) return -1;
    fflush(log->fp);
    log->append_count++;
    return 0;
}

int cnet_fault_append_labeled(CnetFaultLog *log, const CnetFaultRecord *rec,
                              const double *in, const double *tgt) {
    long long ts;
    CnetFaultRecord r;
    if (!log || !log->fp || !rec) return -1;
    if ((!in && !tgt) || rec->in_dim <= 0 || rec->out_dim <= 0)
        return cnet_fault_append(log, rec);
    if (rec->in_dim > CNET_FAULT_MAX_DIM || rec->out_dim > CNET_FAULT_MAX_DIM)
        return -1;
    if (!in || !tgt) return -1;
    r = *rec;
    ts = r.ts_unix;
    if (ts <= 0) ts = (long long)time(NULL);
    if (write_meta_fields(log->fp, &r, ts) < 0) return -1;
    if (write_vec(log->fp, "in", in, r.in_dim) != 0) return -1;
    if (write_vec(log->fp, "tgt", tgt, r.out_dim) != 0) return -1;
    if (fputs("}\n", log->fp) < 0) return -1;
    fflush(log->fp);
    log->append_count++;
    return 0;
}

size_t cnet_fault_count_file(const char *path) {
    FILE *f;
    char *buf;
    size_t n = 0;
    if (!path || !path[0]) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    buf = malloc(CNET_FAULT_LINE_MAX);
    if (!buf) {
        fclose(f);
        return 0;
    }
    while (fgets(buf, CNET_FAULT_LINE_MAX, f)) {
        if (buf[0] == '{') n++;
    }
    free(buf);
    fclose(f);
    return n;
}

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

/* Parse "key":[a,b,c] into out[0..expect-1]. Returns count or -1. */
static int extract_vec(const char *line, const char *key, double *out, int expect) {
    char pat[80];
    const char *p, *end;
    int n = 0;
    char *ep;
    snprintf(pat, sizeof pat, "\"%s\":[", key);
    p = strstr(line, pat);
    if (!p) return -1;
    p += strlen(pat);
    end = strchr(p, ']');
    if (!end) return -1;
    while (p < end && n < expect) {
        while (p < end && (*p == ' ' || *p == ',')) p++;
        if (p >= end) break;
        out[n++] = strtod(p, &ep);
        if (ep == p) break;
        p = ep;
    }
    return n == expect ? n : -1;
}

static void parse_meta(const char *line, CnetFaultRecord *r) {
    char src[32];
    memset(r, 0, sizeof *r);
    extract_ll(line, "ts", &r->ts_unix);
    extract_str(line, "source", src, sizeof src);
    r->source = cnet_fault_source_parse(src);
    extract_str(line, "unit", r->unit, sizeof r->unit);
    extract_str(line, "skill", r->skill, sizeof r->skill);
    extract_str(line, "session", r->session, sizeof r->session);
    extract_int(line, "in_dim", &r->in_dim);
    extract_int(line, "out_dim", &r->out_dim);
    extract_str(line, "label_kind", r->label_kind, sizeof r->label_kind);
    extract_str(line, "note", r->note, sizeof r->note);
}

size_t cnet_fault_load(const char *path, const char *unit_filter,
                       CnetFaultRecord *out, size_t cap) {
    FILE *f;
    char *line;
    size_t n = 0;
    if (!path || !out || !cap) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    line = malloc(CNET_FAULT_LINE_MAX);
    if (!line) {
        fclose(f);
        return 0;
    }
    while (fgets(line, CNET_FAULT_LINE_MAX, f) && n < cap) {
        CnetFaultRecord r;
        if (line[0] != '{') continue;
        parse_meta(line, &r);
        if (unit_filter && unit_filter[0] && strcmp(r.unit, unit_filter) != 0)
            continue;
        out[n++] = r;
    }
    free(line);
    fclose(f);
    return n;
}

size_t cnet_fault_load_vectors(const char *path, const char *unit,
                               int in_dim, int out_dim,
                               double *inputs, double *targets, size_t cap) {
    FILE *f;
    char *line;
    size_t n = 0;
    if (!path || !unit || !unit[0] || !inputs || !targets || !cap) return 0;
    if (in_dim <= 0 || out_dim <= 0 || in_dim > CNET_FAULT_MAX_DIM ||
        out_dim > CNET_FAULT_MAX_DIM)
        return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    line = malloc(CNET_FAULT_LINE_MAX);
    if (!line) {
        fclose(f);
        return 0;
    }
    while (fgets(line, CNET_FAULT_LINE_MAX, f) && n < cap) {
        CnetFaultRecord r;
        double *in_row, *tg_row;
        if (line[0] != '{') continue;
        parse_meta(line, &r);
        if (strcmp(r.unit, unit) != 0) continue;
        if (r.in_dim != in_dim || r.out_dim != out_dim) continue;
        in_row = inputs + n * (size_t)in_dim;
        tg_row = targets + n * (size_t)out_dim;
        if (extract_vec(line, "in", in_row, in_dim) < 0) continue;
        if (extract_vec(line, "tgt", tg_row, out_dim) < 0) continue;
        n++;
    }
    free(line);
    fclose(f);
    return n;
}


/* Dedupe recent labeled faults (same unit+in vector). */
#define CNET_FAULT_DEDUP_CAP 8192
static struct {
    unsigned long long h;
    int used;
} g_dedup[CNET_FAULT_DEDUP_CAP];
static size_t g_dedup_i;
static int g_dedup_seeded;

static unsigned long long fault_hash(const char *unit, const double *in, int in_dim) {
    unsigned long long h = 14695981039346656037ULL;
    const unsigned char *u = (const unsigned char *)(unit ? unit : "");
    int i;
    for (; *u; u++) { h ^= *u; h *= 1099511628211ULL; }
    for (i = 0; i < in_dim; i++) {
        union { double d; unsigned long long u; } x;
        x.d = in[i];
        h ^= x.u + (unsigned long long)i * 0x9e3779b97f4a7c15ULL;
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;
}

static void fault_dedup_insert(unsigned long long h) {
    size_t i;
    for (i = 0; i < CNET_FAULT_DEDUP_CAP; i++)
        if (g_dedup[i].used && g_dedup[i].h == h) return;
    g_dedup[g_dedup_i].h = h;
    g_dedup[g_dedup_i].used = 1;
    g_dedup_i = (g_dedup_i + 1) % CNET_FAULT_DEDUP_CAP;
}

/* Seed the dedupe set from records already on disk.
 *
 * The dedupe set used to be purely in-process, so every fresh
 * cnet_cert_learn_tick / struct_mine_persist invocation started empty and
 * re-appended its whole seed batch. That is how logs/cnet_faults.jsonl reached
 * 4385 lines holding only 311 distinct (unit, input) pairs. Seeding from the
 * existing log makes the bus idempotent across processes.
 *
 * Parsing is deliberately minimal string scanning rather than a JSON parser:
 * the writer's own format is fixed, and "in":[ disambiguates from "in_dim":.
 */
static void fault_dedup_seed_from_log(const char *path) {
    FILE *fp;
    char *line;
    double *vec;
    if (g_dedup_seeded) return;
    g_dedup_seeded = 1;
    if (!path || !path[0]) return;
    fp = fopen(path, "r");
    if (!fp) return;
    line = (char *)malloc(CNET_FAULT_LINE_MAX);
    vec = (double *)malloc(sizeof(double) * CNET_FAULT_MAX_DIM);
    if (!line || !vec) {
        free(line);
        free(vec);
        fclose(fp);
        return;
    }
    while (fgets(line, CNET_FAULT_LINE_MAX, fp)) {
        char unit[128];
        const char *u, *p;
        char *end;
        size_t ul = 0;
        int n = 0;

        if (!strchr(line, '\n') && !feof(fp)) {
            int c; /* over-long line: discard the remainder */
            while ((c = fgetc(fp)) != EOF && c != '\n') { }
            continue;
        }
        u = strstr(line, "\"unit\":\"");
        if (!u) continue;
        u += 8;
        while (*u && *u != '"' && ul + 1 < sizeof unit) unit[ul++] = *u++;
        unit[ul] = '\0';

        p = strstr(line, "\"in\":[");
        if (!p) continue;
        p += 6;
        while (*p && *p != ']' && n < CNET_FAULT_MAX_DIM) {
            double d = strtod(p, &end);
            if (end == p) break;
            vec[n++] = d;
            p = end;
            while (*p == ',' || *p == ' ') p++;
        }
        if (n > 0) fault_dedup_insert(fault_hash(unit, vec, n));
    }
    free(line);
    free(vec);
    fclose(fp);
}

static int fault_dedup_check_add(unsigned long long h) {
    size_t i;
    const char *off = getenv("CNET_FAULT_DEDUPE");
    if (off && off[0] == '0' && off[1] == '\0') return 0; /* dedupe off */
    for (i = 0; i < CNET_FAULT_DEDUP_CAP; i++)
        if (g_dedup[i].used && g_dedup[i].h == h) return 1; /* duplicate */
    fault_dedup_insert(h);
    return 0;
}

void cnet_fault_mirror_kind(const char *unit, const double *input,
                            const double *target, int in_dim, int out_dim,
                            const char *source_name, const char *label_kind,
                            const char *note) {
    const char *fl, *mir;
    CnetFaultLog log;
    CnetFaultRecord rec;
    mir = getenv("CNET_FAULT_MIRROR");
    if (mir && mir[0] == '0' && mir[1] == '\0') return; /* explicit off */
    fl = getenv("CNET_FAULT_LOG");
    if (!fl || !fl[0]) return;
    if (!unit || !input || !target || in_dim <= 0 || out_dim <= 0) return;
    fault_dedup_seed_from_log(fl);
    {
        unsigned long long h = fault_hash(unit, input, in_dim);
        if (fault_dedup_check_add(h)) {
            extern void cnet_acct_add_fault_dedup_skip(void) __attribute__((weak));
            if (cnet_acct_add_fault_dedup_skip) cnet_acct_add_fault_dedup_skip();
            return;
        }
    }
    if (cnet_fault_open(&log, fl) != 0) return;
    memset(&rec, 0, sizeof rec);
    rec.source = cnet_fault_source_parse(source_name);
    if (rec.source == CNET_FAULT_SRC_UNKNOWN) rec.source = CNET_FAULT_SRC_JTC;
    snprintf(rec.unit, sizeof rec.unit, "%s", unit);
    rec.in_dim = in_dim;
    rec.out_dim = out_dim;
    snprintf(rec.label_kind, sizeof rec.label_kind, "%s",
             label_kind && label_kind[0] ? label_kind : "argmax");
    snprintf(rec.note, sizeof rec.note, "%s",
             note && note[0] ? note : "mirror_labeled");
    (void)cnet_fault_append_labeled(&log, &rec, input, target);
    cnet_fault_close(&log);
}

void cnet_fault_mirror_labeled(const char *unit, const double *input,
                               const double *target, int in_dim, int out_dim,
                               const char *source_name) {
    cnet_fault_mirror_kind(unit, input, target, in_dim, out_dim, source_name,
                           "argmax", "mirror_labeled");
}
