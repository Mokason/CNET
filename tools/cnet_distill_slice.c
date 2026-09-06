/* Bounded teacher proposals. No training, certification or admission occurs here. */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../include/cnet_json_internal.h"
#include "../src/roe/cnet_roe_process.h"

#define MAX_Q 32
#define QLEN 512
#define ALEN 4096
#define LINE_MAX_BYTES 8192

static int fail(const char *reason) {
    fprintf(stderr, "DISTILL_SLICE_REFUSED reason=%s\n", reason);
    return 1;
}

static int identifier(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n > 64) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-')) return 0;
    return 1;
}

static int escape_json(const char *s, char *out, size_t cap) {
    size_t used = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (used + 7 >= cap) return -1;
        if (c == '"' || c == '\\') { out[used++] = '\\'; out[used++] = (char)c; }
        else if (c < 32) used += (size_t)snprintf(out + used, cap - used, "\\u%04x", c);
        else out[used++] = (char)c;
    }
    out[used] = 0;
    return 0;
}

static void write_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 32) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static int valid_json(const char *text) {
    JsonCursor c = {(const unsigned char *)text};
    if (json_value(&c, 0)) return 0;
    json_ws(&c);
    return *c.p == 0;
}

/* Retrieve exactly one selected object field; duplicate authority fields refuse. */
static int member(JsonCursor object, const char *name, JsonCursor *value) {
    int found = 0;
    json_ws(&object);
    if (*object.p++ != '{') return -1;
    json_ws(&object);
    if (*object.p == '}') return 0;
    for (;;) {
        char key[128];
        if (json_string(&object, key, sizeof key)) return -1;
        json_ws(&object);
        if (*object.p++ != ':') return -1;
        json_ws(&object);
        if (!strcmp(key, name)) {
            if (found++) return -1;
            *value = object;
        }
        if (json_value(&object, 0)) return -1;
        json_ws(&object);
        if (*object.p == '}') return found;
        if (*object.p++ != ',') return -1;
        json_ws(&object);
    }
}

static int teacher_content(const char *response, char *answer, size_t cap) {
    JsonCursor root = {(const unsigned char *)response}, choices, choice, message, content, end, optional;
    if (!valid_json(response) || member(root, "error", &optional) != 0 ||
        member(root, "choices", &choices) != 1 || *choices.p++ != '[') return -1;
    json_ws(&choices);
    choice = choices;
    if (json_value(&choices, 0)) return -1;
    json_ws(&choices);
    if (*choices.p != ']') return -1; /* exactly one completion */
    if (member(choice, "message", &message) != 1 ||
        member(message, "content", &content) != 1 ||
        json_string(&content, answer, cap)) return -1;
    int has_finish = member(choice, "finish_reason", &optional);
    if (has_finish < 0) return -1;
    if (has_finish) {
        char reason[32];
        if (json_string(&optional, reason, sizeof reason) || strcmp(reason, "stop")) return -1;
    }
    end = (JsonCursor){(const unsigned char *)answer};
    while (*end.p && isspace(*end.p)) end.p++;
    return *end.p ? 0 : -1;
}

static int teacher_ask(const char *q, char *out, size_t cap, int timeout_ms) {
    const char *http = getenv("CNET_STAGE_HTTP"), *model = getenv("CNET_STAGE_MODEL");
    char url[1024], qe[QLEN * 6 + 1], me[256 * 6 + 1], body[sizeof qe + sizeof me + 256];
    char response[32768];
    if (!http || !*http) http = "http://127.0.0.1:8081";
    if (!model || !*model) model = "held";
    if ((strncmp(http, "http://", 7) && strncmp(http, "https://", 8)) ||
        strlen(model) >= 256 || strlen(http) > 900 || strchr(http, '@')) return -1;
    if (escape_json(q, qe, sizeof qe) || escape_json(model, me, sizeof me)) return -1;
    if (snprintf(url, sizeof url, "%s/v1/chat/completions", http) >= (int)sizeof url ||
        snprintf(body, sizeof body,
                 "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],"
                 "\"max_tokens\":128,\"temperature\":0.2}", me, qe) >= (int)sizeof body ||
        !valid_json(body)) return -1;
    char *argv[] = {"curl", "--disable", "--silent", "--show-error", "--fail",
                    "--proto", "=http,https", "--noproxy", "*", "--max-time", "25",
                    "-H", "Content-Type: application/json", "--data-binary", body,
                    "--url", url, NULL};
    if (roe_process_capture_bounded(argv, response, sizeof response, timeout_ms)) return -1;
    return teacher_content(response, out, cap);
}

static int probe(const char *q, int timeout_ms) {
    const char *binary = getenv("CNET_FRONT_DOOR_BIN"), *root = getenv("CNET_PACKS_ROOT");
    char receipt[64];
    if (!binary || !*binary) binary = "bin/roe_front_door";
    if (!root || !*root) return -1;
    char *argv[] = {(char *)binary, "probe", (char *)q, "--root", (char *)root, NULL};
    if (roe_process_capture_bounded(argv, receipt, sizeof receipt, timeout_ms)) return -1;
    if (!strcmp(receipt, "CNET_DISTILL_PROBE_V1 LOCAL\n")) return 1;
    if (!strcmp(receipt, "CNET_DISTILL_PROBE_V1 UNCOVERED\n")) return 0;
    return -1;
}

static int add_query(char qs[][QLEN], int *count, const char *q, int deduplicate) {
    if (!q || !*q || strlen(q) >= QLEN) return -1;
    /* Reuse the UTF-8 validator via a quoted, escaped JSON string. */
    char escaped[QLEN * 6 + 3];
    escaped[0] = '"';
    if (escape_json(q, escaped + 1, sizeof escaped - 2)) return -1;
    strcat(escaped, "\"");
    if (!valid_json(escaped)) return -1;
    if (deduplicate)
        for (int i = 0; i < *count; i++) if (!strcmp(qs[i], q)) return 0;
    if (*count >= MAX_Q) return -1;
    strcpy(qs[(*count)++], q);
    return 0;
}

static int load_queries(const char *path, char qs[][QLEN], int *count, int misslog) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 1024 * 1024) {
        close(fd); return -1;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return -1; }
    char line[LINE_MAX_BYTES];
    size_t used = 0;
    int c, rc = 0;
    do {
        c = fgetc(f);
        if (c == 0 || (c != EOF && c != '\n' && used + 1 >= sizeof line)) { rc = -1; break; }
        if (c != EOF && c != '\n') { line[used++] = (char)c; continue; }
        if (used && line[used - 1] == '\r') used--;
        line[used] = 0;
        char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (*q && (!misslog && *q != '#')) rc = add_query(qs, count, q, 0);
        if (*q && misslog) {
            char decoded[QLEN];
            JsonCursor object = {(const unsigned char *)q}, value;
            if (!valid_json(q) || member(object, "query", &value) != 1 ||
                json_string(&value, decoded, sizeof decoded)) rc = -1;
            else rc = add_query(qs, count, decoded, 1);
        }
        used = 0;
    } while (!rc && c != EOF);
    if (ferror(f)) rc = -1;
    if (fclose(f)) rc = -1;
    return rc;
}

/* Traverse each component using descriptors; never follow an output symlink. */
static int output_directory(const char *path) {
    char copy[1024], *save = NULL, *part;
    if (!path || !*path || strlen(path) >= sizeof copy) return -1;
    strcpy(copy, path);
    int dir = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) return -1;
    int components = 0;
    for (part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) { close(dir); return -1; }
        int created = mkdirat(dir, part, 0700) == 0;
        if ((!created && errno != EEXIST) || (created && fsync(dir))) { close(dir); return -1; }
        int next = openat(dir, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(dir);
        if (next < 0) return -1;
        dir = next;
        components++;
    }
    struct stat st;
    if (!components || fstat(dir, &st) || st.st_uid != geteuid() || (st.st_mode & 0022)) {
        close(dir); return -1;
    }
    return dir;
}

static FILE *new_file(int dir, const char *name) {
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "w");
    if (!f) close(fd);
    return f;
}

static int finish_file(FILE *f) {
    int failed = ferror(f) || fflush(f);
    if (!failed && fsync(fileno(f))) failed = 1;
    if (fclose(f)) failed = 1;
    return failed ? -1 : 0;
}

static int publication_sync(int fd, const char *point) {
#ifdef CNET_DISTILL_TESTING
    const char *injected = getenv("CNET_DISTILL_FAIL_SYNC");
    if (injected && !strcmp(injected, point)) { errno = EIO; return -1; }
#else
    (void)point;
#endif
    return fsync(fd);
}

static int publish(const char *out_root, const char *domain, char qs[][QLEN],
                   char answers[][ALEN], const int *local, int count, int skipped) {
    unsigned char random[16];
    char name[100], hex[33];
    int random_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random_fd < 0) return -1;
    ssize_t nr = read(random_fd, random, sizeof random);
    close(random_fd);
    if (nr != sizeof random) return -1;
    for (size_t i = 0; i < sizeof random; i++) snprintf(hex + 2 * i, 3, "%02x", random[i]);
    snprintf(name, sizeof name, "%s-%s", domain, hex);
    int parent = output_directory(out_root);
    if (parent < 0) return -1;
    if (mkdirat(parent, name, 0700)) { close(parent); return -1; }
    int dir = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) { unlinkat(parent, name, AT_REMOVEDIR); close(parent); return -1; }
    FILE *rows = new_file(dir, "rows.jsonl"), *gold = new_file(dir, "gold_rows.jsonl");
    if (!rows || !gold) {
        if (rows) fclose(rows);
        if (gold) fclose(gold);
        goto failed;
    }
    for (int i = 0; i < count; i++) {
        fputs("{\"domain\":", rows); write_string(rows, domain);
        fputs(",\"q\":", rows); write_string(rows, qs[i]);
        fputs(",\"draft\":", rows); write_string(rows, answers[i]);
        fprintf(rows, ",\"source\":\"%s\",\"claimed_cert\":0,\"auto_cert\":false,"
                      "\"anti_collapse_checked\":true,\"probe\":\"%s\",\"kind\":\"distill_row\"",
                local[i] ? "skipped" : "teacher", local[i] ? "LOCAL" : "UNCOVERED");
        if (local[i]) fputs(",\"skip_reason\":\"anti_collapse_local_tier_a\"", rows);
        fputs("}\n", rows);
        if (!local[i]) {
            fputs("{\"query\":", gold); write_string(gold, qs[i]);
            fputs(",\"answer\":", gold); write_string(gold, answers[i]);
            fputs(",\"auto_cert\":false,\"status\":\"pending_verify\","
                  "\"provenance\":\"distill_slice_teacher\",\"anti_collapse_checked\":true}\n", gold);
        }
    }
    int rows_rc = finish_file(rows), gold_rc = finish_file(gold);
    if (rows_rc || gold_rc) goto failed;
    FILE *manifest = new_file(dir, ".PROPOSE.pending");
    if (!manifest) goto failed;
    fputs("{\"kind\":\"distill_slice\",\"domain\":", manifest); write_string(manifest, domain);
    fprintf(manifest, ",\"n_rows\":%d,\"teacher_ok\":%d,\"gold_rows\":%d,"
                      "\"skipped_anti_collapse\":%d,\"anti_collapse_checked\":true,"
                      "\"dry_run\":false,\"auto_cert\":false,\"status\":\"pending_verify\","
                      "\"student\":\"none_until_verify\",\"law\":\"teacher_proposes_never_self_cert\","
                      "\"ts\":%ld}\n", count, count - skipped, count - skipped, skipped, (long)time(NULL));
    if (finish_file(manifest) || publication_sync(dir, "before_commit")) goto failed;
    /* Atomic no-replace publication marker; incomplete directories have no manifest. */
    if (linkat(dir, ".PROPOSE.pending", dir, "PROPOSE.json", 0)) goto failed;
    if (unlinkat(dir, ".PROPOSE.pending", 0) ||
        publication_sync(dir, "after_commit") || publication_sync(parent, "parent_commit")) {
        /* A visible complete proposal may already be durable. Never tear it down. */
        fprintf(stderr, "DISTILL_SLICE_COMMIT_UNCERTAIN out=%s/%s retained=1\n", out_root, name);
        close(dir);
        close(parent);
        return -2;
    }
    close(dir);
    close(parent);
    printf("DISTILL_SLICE domain=%s rows=%d gold=%d skipped_collapse=%d "
           "collapse_checked=1 teacher_ok=%d dry=0 out=%s/%s auto_cert=0\n",
           domain, count, count - skipped, skipped, count - skipped, out_root, name);
    return 0;
failed:
    (void)unlinkat(dir, ".PROPOSE.pending", 0);
    (void)unlinkat(dir, "rows.jsonl", 0);
    (void)unlinkat(dir, "gold_rows.jsonl", 0);
    close(dir);
    (void)unlinkat(parent, name, AT_REMOVEDIR);
    close(parent);
    return -1;
}

static int selftest(void) {
    char answer[64], escaped[64];
    if (teacher_content("{\"choices\":[{\"message\":{\"content\":\"external\\n\\u03bb\"}}]}", answer, sizeof answer) ||
        strcmp(answer, "external\nλ") ||
        !teacher_content("{\"content\":\"wrong schema\"}", answer, sizeof answer) ||
        !teacher_content("{\"choices\":[{\"message\":{\"content\":\"ok\"}}]} junk", answer, sizeof answer) ||
        escape_json("quote\"\t", escaped, sizeof escaped) ||
        !identifier("bounded_domain") || identifier("../escape")) return fail("selftest");
    puts("DISTILL_SLICE_SELFTEST_PASS");
    return 0;
}

static void usage(void) {
    fputs("usage: cnet_distill_slice --domain NAME --query Q [--query Q ...]\n"
          "       [--from-file FILE] [--from-miss-log FILE] [--out DIR]\n"
          "       [--dry-run|--teacher] [--timeout-ms 1..25000]\n"
          "Teacher publication requires CNET_PACKS_ROOT and a successful local probe.\n"
          "Dry-run validates inputs only; never writes or contacts a process/teacher.\n", stderr);
}

int main(int argc, char **argv) {
    const char *domain = NULL, *out_root = NULL;
    char qs[MAX_Q][QLEN] = {{0}}, answers[MAX_Q][ALEN] = {{0}}, default_out[1024];
    int count = 0, dry = 1, dry_requested = 0, timeout_ms = 25000, local[MAX_Q], skipped = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--domain") && i + 1 < argc) domain = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_root = argv[++i];
        else if (!strcmp(argv[i], "--query") && i + 1 < argc) {
            if (add_query(qs, &count, argv[++i], 0)) return fail("query_bound");
        } else if ((!strcmp(argv[i], "--from-file") || !strcmp(argv[i], "--from-miss-log")) && i + 1 < argc) {
            int misslog = !strcmp(argv[i], "--from-miss-log");
            if (load_queries(argv[++i], qs, &count, misslog)) return fail("query_file");
        } else if (!strcmp(argv[i], "--dry-run")) dry_requested = 1;
        else if (!strcmp(argv[i], "--teacher")) dry = 0;
        else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc) {
            char *end;
            long n = strtol(argv[++i], &end, 10);
            if (!*argv[i] || *end || n < 1 || n > 25000) return fail("timeout_bound");
            timeout_ms = (int)n;
        } else if (!strcmp(argv[i], "--selftest") && argc == 2) return selftest();
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else return fail("unsupported_argument");
    }
    if (!identifier(domain) || !count) { usage(); return fail("domain_or_queries"); }
    if (dry_requested) dry = 1;
    if (!out_root) {
        const char *minimal = getenv("CNET_MINIMAL_ROOT");
        if (!minimal || !*minimal) out_root = "var/capsule_inbox";
        else {
            if (snprintf(default_out, sizeof default_out, "%s/var/capsule_inbox", minimal) >= (int)sizeof default_out)
                return fail("output_bound");
            out_root = default_out;
        }
    }
    if (!*out_root || strlen(out_root) >= 1024) return fail("output_bound");
    if (dry) {
        printf("DISTILL_SLICE domain=%s queries=%d dry=1 collapse_checked=0 teacher_ok=0 auto_cert=0\n", domain, count);
        puts("DISTILL_SLICE_DRY_RUN_PASS");
        return 0;
    }
    for (int i = 0; i < count; i++) {
        local[i] = probe(qs[i], timeout_ms);
        if (local[i] < 0) return fail("anti_collapse_probe");
        skipped += local[i];
    }
    for (int i = 0; i < count; i++)
        if (!local[i] && teacher_ask(qs[i], answers[i], sizeof answers[i], timeout_ms))
            return fail("teacher_receipt");
    int published = publish(out_root, domain, qs, answers, local, count, skipped);
    if (published == -2) return 3;
    if (published) return fail("publication");
    puts("DISTILL_SLICE_PASS");
    return 0;
}
