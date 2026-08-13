/* Structured Hermes task outcomes — envelope classifier + fail-rate fuel (core).
 *
 * Usage:
 *   governor_hermes_structured [--test]
 *
 * Writes logs/governor/hermes_structured.json and prints HERMES_STRUCTURED_OK.
 * SQLite state.db scan is best-effort; missing/empty DB → noisy zero rate.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

static const char *FAIL_MARKERS[] = {
    "tool error", "tool_call failed", "traceback (most recent call last)",
    "exception:", "timeout waiting", "command failed", "\"ok\": false",
    "\"ok\":false", "error running tool", "failed after", NULL
};
static const char *OK_MARKERS[] = {
    "\"ok\": true", "\"ok\":true", "completed successfully",
    "GOVERNOR_QUALITY_PASS", "VERIFY_FAST_PASS", NULL
};

static void ensure_parent(const char *path) {
    char tmp[512];
    size_t i, len;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            MKDIR(tmp);
            tmp[i] = c;
        }
    }
}

static void lower_copy(const char *in, char *out, size_t cap) {
    size_t i;
    for (i = 0; in[i] && i + 1 < cap; i++)
        out[i] = (char)tolower((unsigned char)in[i]);
    out[i] = 0;
}

static int json_str_empty_or_missing(const char *obj, const char *key) {
    char pat[64];
    const char *p, *q;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(obj, pat);
    if (!p) return 1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 1;
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (*p == '"' ) {
        p++;
        q = strchr(p, '"');
        return !q || q == p;
    }
    if (strncmp(p, "null", 4) == 0 || strncmp(p, "false", 5) == 0) return 1;
    if (*p == '{' || *p == '[') {
        /* empty object/array still non-string error → fail in Python if not None/"" */
        if ((*p == '{' && p[1] == '}') || (*p == '[' && p[1] == ']')) return 1;
        return 0;
    }
    return 0;
}

static int json_has_status(const char *obj, const char *want) {
    char pat[64], val[64];
    const char *p, *q;
    size_t n;
    snprintf(pat, sizeof pat, "\"status\"");
    p = strstr(obj, pat);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (*p != '"') return 0;
    p++;
    q = strchr(p, '"');
    if (!q) return 0;
    n = (size_t)(q - p);
    if (n >= sizeof val) n = sizeof val - 1;
    memcpy(val, p, n);
    val[n] = 0;
    for (n = 0; val[n]; n++) val[n] = (char)tolower((unsigned char)val[n]);
    return strcmp(val, want) == 0;
}

static int json_exit_nonzero(const char *obj) {
    const char *p = strstr(obj, "\"exit_code\"");
    int code;
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    code = atoi(p + 1);
    return code != 0;
}

/* Returns "ok" | "fail" | "skip" into out (capacity >= 8). */
void hermes_classify(const char *text, char *out, size_t cap) {
    char low[4096];
    const char *stripped;
    size_t i;
    if (!text) text = "";
    stripped = text;
    while (*stripped == ' ' || *stripped == '\t' || *stripped == '\n') stripped++;
    if (stripped[0] == '{' || stripped[0] == '[') {
        if (json_has_status(stripped, "pending_approval") ||
            json_has_status(stripped, "pending") ||
            json_has_status(stripped, "awaiting_approval") ||
            json_has_status(stripped, "running")) {
            snprintf(out, cap, "skip");
            return;
        }
        if (json_has_status(stripped, "error") || json_has_status(stripped, "blocked") ||
            json_has_status(stripped, "failed") || json_has_status(stripped, "denied") ||
            json_has_status(stripped, "rejected")) {
            snprintf(out, cap, "fail");
            return;
        }
        if (!json_str_empty_or_missing(stripped, "error")) {
            snprintf(out, cap, "fail");
            return;
        }
        if (strstr(stripped, "\"exit_code\"") && json_exit_nonzero(stripped)) {
            snprintf(out, cap, "fail");
            return;
        }
        snprintf(out, cap, "ok");
        return;
    }
    lower_copy(stripped, low, sizeof low);
    for (i = 0; FAIL_MARKERS[i]; i++) {
        if (strstr(low, FAIL_MARKERS[i])) {
            snprintf(out, cap, "fail");
            return;
        }
    }
    for (i = 0; OK_MARKERS[i]; i++) {
        char m[128];
        lower_copy(OK_MARKERS[i], m, sizeof m);
        if (strstr(low, m)) {
            snprintf(out, cap, "ok");
            return;
        }
    }
    snprintf(out, cap, "ok");
}

static int write_report(const char *out_path, int fails, int oks, int skipped,
                        double rate, int noisy, const char *reason) {
    FILE *f;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[64];
    ensure_parent(out_path);
    if (tm)
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S%z", tm);
    else
        snprintf(ts, sizeof ts, "unknown");
    f = fopen(out_path, "w");
    if (!f) return 1;
    fprintf(f,
            "{\n  \"ts\": \"%s\",\n  \"fails\": %d,\n  \"oks\": %d,\n"
            "  \"pending_skipped\": %d,\n  \"hermes_task_fail_rate\": %.4f,\n"
            "  \"noisy\": %s,\n  \"classifier\": \"envelope_v2\"%s%s%s\n}\n",
            ts, fails, oks, skipped, rate, noisy ? "true" : "false",
            reason ? ",\n  \"reason\": \"" : "", reason ? reason : "",
            reason ? "\"" : "");
    fclose(f);
    return 0;
}

static int self_test(void) {
    char v[16];
    hermes_classify("{\"output\":\"hello\",\"error\":\"\",\"exit_code\":0}", v, sizeof v);
    if (strcmp(v, "ok") != 0) return 1;
    hermes_classify("{\"output\":\"grep error\",\"error\":\"\",\"exit_code\":0}", v, sizeof v);
    if (strcmp(v, "ok") != 0) return 1;
    hermes_classify("{\"output\":\"PERSONALITY_SELFTEST_PASS checks=6\",\"error\":\"\",\"exit_code\":0}",
                    v, sizeof v);
    if (strcmp(v, "ok") != 0) return 1;
    hermes_classify("{\"output\":\"\",\"error\":\"BLOCKED\",\"exit_code\":-1}", v, sizeof v);
    if (strcmp(v, "fail") != 0) return 1;
    hermes_classify("{\"output\":\"\",\"error\":\"\",\"exit_code\":1}", v, sizeof v);
    if (strcmp(v, "fail") != 0) return 1;
    hermes_classify("{\"status\":\"pending_approval\",\"error\":\"\",\"exit_code\":-1}", v,
                    sizeof v);
    if (strcmp(v, "skip") != 0) return 1;
    hermes_classify("Traceback (most recent call last):\n  File", v, sizeof v);
    if (strcmp(v, "fail") != 0) return 1;
    hermes_classify("plain tool output, nothing wrong", v, sizeof v);
    if (strcmp(v, "ok") != 0) return 1;
    printf("HERMES_STRUCTURED_SELFTEST_PASS checks=8\n");
    return 0;
}

int main(int argc, char **argv) {
    const char *gov, *db;
    char out_path[512];
    double hours, rate;
    int fails = 0, oks = 0, skipped = 0, noisy = 1, i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) return self_test();
    }
    gov = getenv("CNET_GOVERNOR_DIR");
    if (!gov || !gov[0]) gov = "logs/governor";
    snprintf(out_path, sizeof out_path, "%s/hermes_structured.json", gov);
    hours = getenv("HERMES_STRUCT_HOURS") ? atof(getenv("HERMES_STRUCT_HOURS")) : 12.0;
    (void)hours;
    db = getenv("HERMES_STATE_DB");
    /* Core path without linking sqlite: missing DB → honest noisy zero. */
    if (!db || !db[0]) {
        write_report(out_path, 0, 0, 0, 0.0, 1, "db_missing_or_empty");
        printf("HERMES_STRUCTURED_OK {\"fail_rate\":0.0,\"fails\":0,\"oks\":0,\"noisy\":true}\n");
        return 0;
    }
    {
        FILE *f = fopen(db, "rb");
        long sz = 0;
        if (f) {
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            fclose(f);
        }
        if (sz < 1000) {
            write_report(out_path, 0, 0, 0, 0.0, 1, "db_missing_or_empty");
            printf("HERMES_STRUCTURED_OK {\"fail_rate\":0.0,\"fails\":0,\"oks\":0,\"noisy\":true}\n");
            return 0;
        }
    }
    /* Full sqlite scan WITHHELD in this C core (no sqlite3 link). Rate stays noisy. */
    rate = 0.0;
    noisy = 1;
    write_report(out_path, fails, oks, skipped, rate, noisy, "sqlite_scan_withheld");
    printf("HERMES_STRUCTURED_OK {\"fail_rate\":%.4f,\"fails\":%d,\"oks\":%d,\"noisy\":true,"
           "\"note\":\"WITHHELD sqlite\"}\n",
           rate, fails, oks);
    return 0;
}
