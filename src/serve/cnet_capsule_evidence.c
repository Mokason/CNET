#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cnet_capsule_evidence.h"
#include "cce/cce_campaign_provenance.h"
#include "cnet_json_internal.h"
#include "../roe/cnet_roe_process.h"
#include <ctype.h>
#include <stdio.h>
#include <sys/stat.h>

#define SOURCE_LIMIT (1024u * 1024u)
#define ASSET_LIMIT 16384u
#define LINE_LIMIT 512u
#define EXTRACTOR "cnet_literal_define_v1"
#define RECEIPT_TOOL "/usr/bin/grep"
typedef struct { const char *name, *path, *macro; int string_value; } FactSpec;
static const FactSpec specs[CNET_CAPSULE_EVIDENCE_FACTS] = {
    {"capsule-schema", "include/cnet_capsule.h", "CNET_CAPSULE_SCHEMA", 0},
    {"capsule-asset-schema", "include/cnet_capsule.h", "CNET_CAPSULE_SCHEMA_ASSET", 0},
    {"capsule-asset-file", "include/cnet_capsule.h", "CNET_CAPSULE_ASSET_FILE", 1},
    {"capsule-reason-limit", "include/cnet_capsule.h", "CNET_CAPSULE_REASON_MAX", 0},
    {"json-depth-limit", "include/cnet_json_internal.h", "CNETD_JSON_DEPTH_MAX", 0}
};
typedef struct {
    char value[65], sha256[65], receipt[LINE_LIMIT + 32], text[CNET_CAPSULE_EVIDENCE_TEXT];
} Fact;
struct CnetCapsuleEvidence { Fact facts[CNET_CAPSULE_EVIDENCE_FACTS]; char sha256[65], tool_sha256[65]; };
static int refuse(char *error, size_t cap, const char *why) {
    if (error && cap) snprintf(error, cap, "%s", why);
    return -1;
}
static int copy_text(const char *text, char *out, size_t cap) {
    if (!out || !cap) return -1;
    out[0] = 0;
    if (!text || strlen(text) >= cap) return -1;
    memcpy(out, text, strlen(text) + 1);
    return 0;
}
const char *cnet_capsule_evidence_fact_name(unsigned i) {
    return i < CNET_CAPSULE_EVIDENCE_FACTS ? specs[i].name : NULL;
}
int cnet_capsule_evidence_request(const char *name, char *out, size_t cap) {
    if (out && cap) out[0] = 0;
    if (!name) return -1;
    for (unsigned i = 0; i < CNET_CAPSULE_EVIDENCE_FACTS; i++) {
        if (!strcmp(name, specs[i].name)) {
            char text[128];
            snprintf(text, sizeof text, "capsule %s %s %u", CNET_CAPSULE_EVIDENCE_INPUT, CNET_CAPSULE_EVIDENCE_OUTPUT, i);
            return copy_text(text, out, cap);
        }
    }
    return -1;
}
int cnet_capsule_evidence_render(const CnetCapsuleEvidence *e, unsigned value, char *out, size_t cap) {
    return copy_text(e && value < CNET_CAPSULE_EVIDENCE_FACTS ? e->facts[value].text : NULL, out, cap);
}
int cnet_capsule_evidence_identity(const CnetCapsuleEvidence *e, char out[65]) {
    return copy_text(e ? e->sha256 : NULL, out, 65);
}
void cnet_capsule_evidence_close(CnetCapsuleEvidence *e) { free(e); }
int cnet_capsule_evidence_compatible(const CnetCapsuleEvidence *a, const CnetCapsuleEvidence *b) {
    if (!a || !b) return -1;
    /* This version accepts only one complete output signature. */
    for (unsigned i = 0; i < CNET_CAPSULE_EVIDENCE_FACTS; i++)
        if (strcmp(a->facts[i].text, b->facts[i].text)) return -1;
    return 0;
}

static int source_node(int fd, int directory) {
    struct stat st;
    return fstat(fd, &st) || st.st_uid != geteuid() ||
        (directory ? !S_ISDIR(st.st_mode) : (!S_ISREG(st.st_mode) || st.st_nlink != 1)) ? -1 : 0;
}
/* Ancestors are traversed, never realpath-resolved; the configured root itself
 * and all descendants must be owner-owned. Source is untrusted read-only data,
 * so group-writable checkouts are permitted. Outputs use a separate policy. */
static int open_root(const char *root) {
    char path[4096];
    if (!root || root[0] != '/' || strlen(root) >= sizeof path || !root[1]) return -1;
    strcpy(path, root + 1);
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    char *part = path;
    while (part && *part) {
        char *slash = strchr(part, '/');
        if (slash) *slash = 0;
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) { close(fd); return -1; }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd); fd = next;
        if (fd < 0) return -1;
        part = slash ? slash + 1 : NULL;
        if (part && !*part) { close(fd); return -1; }
    }
    if (source_node(fd, 1)) { close(fd); return -1; }
    return fd;
}
static int read_source(const char *root, const char *path, char **out, size_t *length) {
    *out = NULL; *length = 0;
    /* Relative paths come only from the compiled domain allowlist. */
    int rootfd = open_root(root);
    if (rootfd < 0) return -1;
    int dir = openat(rootfd, "include", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(rootfd);
    if (dir < 0) return -1;
    if (source_node(dir, 1) || strncmp(path, "include/", 8) || strchr(path + 8, '/')) { close(dir); return -1; }
    int fd = openat(dir, path + 8, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    close(dir);
    if (fd < 0) return -1;
    struct stat before, after;
    if (source_node(fd, 0) || fstat(fd, &before) || before.st_size <= 0 || before.st_size > SOURCE_LIMIT) { close(fd); return -1; }
    size_t n = (size_t)before.st_size, used = 0;
    char *data = malloc(n + 1);
    if (!data) { close(fd); return -1; }
    while (used < n) {
        ssize_t got = read(fd, data + used, n - used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        used += (size_t)got;
    }
    char extra;
    int bad = used != n || read(fd, &extra, 1) != 0 || fstat(fd, &after) ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
        memchr(data, 0, n) != NULL;
    close(fd);
    if (bad) { free(data); return -1; }
    data[n] = 0; *out = data; *length = n;
    return 0;
}
static int literal(const FactSpec *spec, const char *raw, char value[65]) {
    size_t n = strlen(raw);
    if (spec->string_value) {
        if (n < 3 || n > 66 || raw[0] != '"' || raw[n - 1] != '"') return -1;
        for (size_t i = 1; i + 1 < n; i++)
            if (!((raw[i] >= 'A' && raw[i] <= 'Z') || (raw[i] >= 'a' && raw[i] <= 'z') ||
                  (raw[i] >= '0' && raw[i] <= '9') || raw[i] == '_' || raw[i] == '-' || raw[i] == '.')) return -1;
        if (n - 2 >= 65) return -1;
        memcpy(value, raw + 1, n - 2); value[n - 2] = 0;
        if (!strcmp(value, ".") || !strcmp(value, "..")) return -1;
        return 0;
    }
    if (!n || n > 11 || (raw[0] == '0' && n > 1 && raw[1] != 'u')) return -1;
    unsigned long value_n = 0;
    size_t i = 0;
    for (; i < n && raw[i] >= '0' && raw[i] <= '9'; i++) {
        value_n = value_n * 10 + (unsigned)(raw[i] - '0');
        if (value_n > 1000000u) return -1;
    }
    if (!i || (i != n && !(i + 1 == n && raw[i] == 'u'))) return -1;
    snprintf(value, 65, "%lu", value_n);
    return 0;
}
static int extract(unsigned index, const char *data, size_t length, Fact *fact, char exact[LINE_LIMIT]) {
    const FactSpec *spec = &specs[index];
    unsigned line_no = 1, matches = 0;
    const char *p = data, *end = data + length;
    for (; p < end; line_no++) {
        const char *newline = memchr(p, '\n', (size_t)(end - p));
        const char *last = newline ? newline : end;
        size_t n = (size_t)(last - p);
        if (n >= LINE_LIMIT) return -1;
        char line[LINE_LIMIT]; memcpy(line, p, n); line[n] = 0;
        char directive[32], macro[128], raw[128], extra;
        int fields = sscanf(line, "%31s %127s %127s %c", directive, macro, raw, &extra);
        if (fields >= 2 && !strcmp(directive, "#define") && !strcmp(macro, spec->macro)) {
            if (++matches != 1 || fields != 3 || literal(spec, raw, fact->value)) return -1;
            /* Canonical extraction deliberately refuses indentation, comments,
             * macro expressions, escapes and preprocessor interpretation. */
            if (strncmp(line, "#define ", 8)) return -1;
            strcpy(exact, line);
            snprintf(fact->receipt, sizeof fact->receipt, "%u:%s\n", line_no, line);
        }
        p = newline ? newline + 1 : end;
    }
    if (matches != 1 || cce_sha256_bytes_hex(data, length, fact->sha256)) return -1;
    int n = snprintf(fact->text, sizeof fact->text, "%s:%s=%s", spec->path, spec->macro, fact->value);
    return n < 0 || (size_t)n >= sizeof fact->text ? -1 : 0;
}
int cnet_capsule_evidence_fresh(const CnetCapsuleEvidence *e, const char *root, char *error, size_t cap) {
    if (error && cap) error[0] = 0;
    if (!e) return refuse(error, cap, "source_evidence_missing");
    for (unsigned i = 0; i < CNET_CAPSULE_EVIDENCE_FACTS; i++) {
        char *data = NULL, line[LINE_LIMIT]; size_t length = 0; Fact current = {0};
        if (read_source(root, specs[i].path, &data, &length)) return refuse(error, cap, "source_path_ownership_or_read");
        int bad = extract(i, data, length, &current, line);
        free(data);
        if (bad || strcmp(current.sha256, e->facts[i].sha256) || strcmp(current.receipt, e->facts[i].receipt) ||
            strcmp(current.text, e->facts[i].text)) return refuse(error, cap, "source_changed_or_literal_mismatch");
    }
    return 0;
}

static int exact_port(const Port *p, const char *tag) {
    return p && p->family == PORT_BINARY_MSB && p->field_width == 3 && p->field_count == 1 && !strcmp(p->tag, tag);
}
static int exact_rows(const double *in, const double *out, size_t count) {
    unsigned seen = 0;
    if (!in || !out || count != CNET_CAPSULE_EVIDENCE_FACTS) return -1;
    for (size_t i = 0; i < count; i++) {
        unsigned value = 0;
        for (unsigned b = 0; b < 3; b++) {
            double x = in[i * 3 + b];
            if ((x != 0 && x != 1) || out[i * 3 + b] != x) return -1;
            value = value * 2 + (unsigned)x;
        }
        if (value >= count || (seen & (1u << value))) return -1;
        seen |= 1u << value;
    }
    return seen == 31 ? 0 : -1;
}
static int binding(const Contract *c, const HybridCoverage *h) {
    return !c || !h || c->input_port_count != 1 || c->output_port_count != 1 ||
        !exact_port(&c->input_ports[0], CNET_CAPSULE_EVIDENCE_INPUT) || !exact_port(&c->output_ports[0], CNET_CAPSULE_EVIDENCE_OUTPUT) ||
        exact_rows(c->inputs, c->outputs, c->exemplar_count) || !h->active || h->generalizes ||
        strcmp(c->name, h->unit) || h->in_dim != 3 || h->out_dim != 3 ||
        !exact_port(&h->input_port, CNET_CAPSULE_EVIDENCE_INPUT) || !exact_port(&h->goal_port, CNET_CAPSULE_EVIDENCE_OUTPUT) ||
        exact_rows(h->rows, h->targets, h->n_rows) ? -1 : 0;
}
/* Fixed ordered JSON object reader. Requiring this one canonical field order
 * removes duplicate/unknown-field ambiguities and bounds parser complexity. */
static int token(JsonCursor *j, unsigned char c) {
    json_ws(j);
    if (*j->p != c) return -1;
    j->p++; return 0;
}
static int field(JsonCursor *j, const char *key, char *out, size_t cap, int comma) {
    char actual[32];
    if ((comma && token(j, ',')) || json_string(j, actual, sizeof actual) || strcmp(key, actual) || token(j, ':')) return -1;
    json_ws(j); return json_string(j, out, cap);
}
static int fixed_field(JsonCursor *j, const char *key, const char *wanted, int comma) {
    char actual[256];
    return field(j, key, actual, sizeof actual, comma) || strcmp(actual, wanted) ? -1 : 0;
}
static int sha256_text(const char *s) {
    if (strlen(s) != 64) return -1;
    for (unsigned i = 0; i < 64; i++) if (!strchr("0123456789abcdef", s[i])) return -1;
    return 0;
}
CnetCapsuleEvidence *cnet_capsule_evidence_parse(const void *asset, size_t length, unsigned schema,
        const Contract *contract, const HybridCoverage *coverage, char *error, size_t cap) {
    if (error && cap) error[0] = 0;
    if (!asset || !length || length > ASSET_LIMIT || schema != CNET_CAPSULE_EVIDENCE_SCHEMA ||
        memchr(asset, 0, length) || binding(contract, coverage)) {
        refuse(error, cap, "source_asset_schema_or_label_binding"); return NULL;
    }
    char *text = malloc(length + 1);
    CnetCapsuleEvidence *e = calloc(1, sizeof *e);
    if (!text || !e) { free(text); free(e); refuse(error, cap, "source_asset_memory"); return NULL; }
    memcpy(text, asset, length); text[length] = 0;
    JsonCursor syntax = {(const unsigned char *)text};
    if (json_value(&syntax, 0)) goto bad;
    json_ws(&syntax); if (*syntax.p) goto bad;
    JsonCursor j = {(const unsigned char *)text};
    if (token(&j, '{') || fixed_field(&j, "extractor", EXTRACTOR, 0) ||
        fixed_field(&j, "tool", RECEIPT_TOOL, 1) || field(&j, "tool_sha256", e->tool_sha256, sizeof e->tool_sha256, 1) ||
        sha256_text(e->tool_sha256) || fixed_field(&j, "input", CNET_CAPSULE_EVIDENCE_INPUT, 1) ||
        fixed_field(&j, "output", CNET_CAPSULE_EVIDENCE_OUTPUT, 1) || token(&j, ',')) goto bad;
    char key[32]; json_ws(&j);
    if (json_string(&j, key, sizeof key) || strcmp(key, "facts") || token(&j, ':') || token(&j, '[')) goto bad;
    for (unsigned i = 0; i < CNET_CAPSULE_EVIDENCE_FACTS; i++) {
        Fact *f = &e->facts[i];
        if ((i && token(&j, ',')) || token(&j, '{') || fixed_field(&j, "name", specs[i].name, 0) ||
            fixed_field(&j, "path", specs[i].path, 1) || fixed_field(&j, "macro", specs[i].macro, 1) ||
            field(&j, "value", f->value, sizeof f->value, 1) || field(&j, "sha256", f->sha256, sizeof f->sha256, 1) ||
            field(&j, "receipt", f->receipt, sizeof f->receipt, 1) || field(&j, "text", f->text, sizeof f->text, 1) || token(&j, '}')) goto bad;
        if (sha256_text(f->sha256)) goto bad;
        char *colon = strchr(f->receipt, ':');
        if (!colon || colon == f->receipt || (size_t)(colon - f->receipt) > 7 || f->receipt[0] == '0') goto bad;
        for (const char *p = f->receipt; p != colon; p++) if (*p < '0' || *p > '9') goto bad;
        /* Replay the claimed line independent of its source line number. */
        Fact replay = {0}; char exact[LINE_LIMIT];
        if (extract(i, colon + 1, strlen(colon + 1), &replay, exact) || strcmp(f->value, replay.value) || strcmp(f->text, replay.text)) goto bad;
        size_t line_len = strlen(exact);
        if (strlen(colon + 1) != line_len + 1 || colon[1 + line_len] != '\n') goto bad;
    }
    if (token(&j, ']') || token(&j, '}')) goto bad;
    json_ws(&j); if (*j.p || cce_sha256_bytes_hex(asset, length, e->sha256)) goto bad;
    free(text); return e;
bad:
    free(text); free(e); refuse(error, cap, "source_asset_malformed_or_decoder"); return NULL;
}

static int emit_string(FILE *f, const char *s) {
    if (fputc('"', f) == EOF) return -1;
    for (; *s; s++) {
        if (*s == '\n') { if (fputs("\\n", f) == EOF) return -1; }
        else if (*s == '\t') { if (fputs("\\t", f) == EOF) return -1; }
        else {
            if ((*s == '"' || *s == '\\') && fputc('\\', f) == EOF) return -1;
            if ((unsigned char)*s < 32 || fputc(*s, f) == EOF) return -1;
        }
    }
    return fputc('"', f) == EOF ? -1 : 0;
}
static int emit_field(FILE *f, const char *key, const char *value, int comma) {
    return (comma && fputc(',', f) == EOF) || emit_string(f, key) || fputc(':', f) == EOF || emit_string(f, value) ? -1 : 0;
}
int cnet_capsule_evidence_acquire(const char *root, void **asset, size_t *length, char *error, size_t cap) {
    if (error && cap) error[0] = 0;
    if (!asset || !length) return refuse(error, cap, "source_acquire_arguments");
    *asset = NULL; *length = 0;
    CnetCapsuleEvidence e = {0};
    char scratch[] = "/tmp/cnet-source-check-XXXXXX", snapshot[128] = "";
    if (!mkdtemp(scratch)) return refuse(error, cap, "source_receipt_stage");
    snprintf(snapshot, sizeof snapshot, "%s/source", scratch);
    int rc = -1;
    /* A recorded executable identity, not a signature or compiler proof. */
    if (cce_sha256_file_hex(RECEIPT_TOOL, e.tool_sha256)) goto done;
    for (unsigned i = 0; i < CNET_CAPSULE_EVIDENCE_FACTS; i++) {
        char *data = NULL, line[LINE_LIMIT], actual[LINE_LIMIT + 32]; size_t n = 0;
        if (read_source(root, specs[i].path, &data, &n)) goto done;
        if (extract(i, data, n, &e.facts[i], line)) { free(data); goto done; }
        int fd = open(snapshot, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) { free(data); goto done; }
        size_t used = 0;
        while (used < n) {
            ssize_t written = write(fd, data + used, n - used);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) break;
            used += (size_t)written;
        }
        free(data);
        int failed = used != n;
        if (close(fd)) failed = 1;
        if (failed) goto done;
        char *argv[] = {RECEIPT_TOOL, "--binary-files=without-match", "-n", "-F", "-x", "--", line, snapshot, NULL};
        if (roe_process_capture_bounded(argv, actual, sizeof actual, 5000) || strcmp(actual, e.facts[i].receipt)) goto done;
        if (unlink(snapshot)) goto done;
    }
    char final_tool[65];
    if (cce_sha256_file_hex(RECEIPT_TOOL, final_tool) || strcmp(final_tool, e.tool_sha256) ||
        cnet_capsule_evidence_fresh(&e, root, error, cap)) goto done;
    char *bytes = NULL; size_t n = 0;
    FILE *f = open_memstream(&bytes, &n);
    if (!f) goto done;
    int bad = fputc('{', f) == EOF || emit_field(f, "extractor", EXTRACTOR, 0) || emit_field(f, "tool", RECEIPT_TOOL, 1) ||
        emit_field(f, "tool_sha256", e.tool_sha256, 1) ||
        emit_field(f, "input", CNET_CAPSULE_EVIDENCE_INPUT, 1) || emit_field(f, "output", CNET_CAPSULE_EVIDENCE_OUTPUT, 1) || fputs(",\"facts\":[", f) == EOF;
    for (unsigned i = 0; !bad && i < CNET_CAPSULE_EVIDENCE_FACTS; i++) {
        Fact *v = &e.facts[i];
        bad = (i && fputc(',', f) == EOF) || fputc('{', f) == EOF || emit_field(f, "name", specs[i].name, 0) ||
            emit_field(f, "path", specs[i].path, 1) || emit_field(f, "macro", specs[i].macro, 1) ||
            emit_field(f, "value", v->value, 1) || emit_field(f, "sha256", v->sha256, 1) || emit_field(f, "receipt", v->receipt, 1) ||
            emit_field(f, "text", v->text, 1) || fputc('}', f) == EOF;
    }
    bad |= fputs("]}", f) == EOF;
    if (fclose(f)) bad = 1;
    if (bad || !n || n > ASSET_LIMIT) { free(bytes); goto done; }
    *asset = bytes; *length = n; rc = 0;
done:
    unlink(snapshot); rmdir(scratch);
    if (rc && (!error || !cap || !error[0])) refuse(error, cap, "source_read_literal_or_tool_receipt");
    return rc;
}
