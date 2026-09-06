/* cnet_ingest_info — absorb information as uncertified knowledge chunks.
 * Not CERT. Not hardcoded FAQ.
 *
 * usage:
 *   cnet_ingest_info --source NAME --text "...."
 *   cnet_ingest_info --source NAME --file path.txt
 *   echo text | cnet_ingest_info --source stdin
 *   cnet_ingest_info --recall "topic"
 *
 * Store: CNET_KNOWLEDGE_PATH or var/marble_knowledge.jsonl
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define CHUNK 480
#define LINE 2048

static const char *kb_path(void) {
    static char p[512];
    const char *e = getenv("CNET_KNOWLEDGE_PATH");
    const char *min = getenv("CNET_MINIMAL_ROOT");
    if (e && e[0]) return e;
    if (min && min[0]) {
        snprintf(p, sizeof p, "%s/var/marble_knowledge.jsonl", min);
        return p;
    }
    return "var/marble_knowledge.jsonl";
}

static void ensure_parent(const char *path) {
    char d[512];
    char *sl;
    snprintf(d, sizeof d, "%s", path);
    sl = strrchr(d, '/');
    if (sl) {
        *sl = 0;
        mkdir(d, 0755);
    }
}

static void jesc(FILE *f, const char *s) {
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        if (*s != '\n' && *s != '\r') fputc(*s, f);
        else fputc(' ', f);
    }
}

static int contains_ci(const char *h, const char *n) {
    size_t i, j, nl, hl;
    if (!h || !n || !n[0]) return 0;
    nl = strlen(n);
    hl = strlen(h);
    if (nl > hl) return 0;
    for (i = 0; i + nl <= hl; i++) {
        for (j = 0; j < nl; j++) {
            if (tolower((unsigned char)h[i + j]) != tolower((unsigned char)n[j]))
                break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

static int ingest_buf(const char *src, const char *text) {
    const char *path = kb_path();
    FILE *f;
    const char *p = text;
    int n = 0;
    time_t now = time(NULL);
    if (!text || !text[0]) return 0;
    ensure_parent(path);
    f = fopen(path, "a");
    if (!f) return -1;
    while (*p) {
        char chunk[CHUNK + 4];
        size_t i = 0;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        while (*p && i < CHUNK) chunk[i++] = *p++;
        /* prefer break at space */
        if (*p && i == CHUNK) {
            size_t k = i;
            while (k > CHUNK / 2 && !isspace((unsigned char)chunk[k - 1])) k--;
            if (k > CHUNK / 2) {
                p -= (i - k);
                i = k;
            }
        }
        while (i && isspace((unsigned char)chunk[i - 1])) i--;
        chunk[i] = 0;
        if (i < 8) continue;
        fprintf(f, "{\"ts\":%ld,\"source\":\"", (long)now);
        jesc(f, src ? src : "user");
        fprintf(f, "\",\"chunk\":\"");
        jesc(f, chunk);
        fprintf(f, "\",\"claimed_cert\":0,\"kind\":\"ingest\"}\n");
        n++;
    }
    fclose(f);
    return n;
}

/* ---------------- miss-log prose harvest -------------------------------
 *
 * The miss log is where organic demand lands, so it is the right place to
 * mine information from -- but it is overwhelmingly refusals: on this host
 * 17844 of 18198 rows are "ABSTAIN: no local skill...". Dumping it wholesale
 * would fill the knowledge base with CNET saying it does not know things.
 *
 * So harvest is selective and reports what it dropped. Refused:
 *   - abstain / refusal / clarification-request answers  (not information)
 *   - CNET's own self-description                        (ANTI-COLLAPSE: our
 *     own output must never come back as a learned note)
 *   - probe junk matching the promote blocklist          (zzq, xyzzy, ...)
 *   - chunks already in the knowledge base               (dedupe)
 *
 * Everything kept is still claimed_cert=0. Ingest is not CERT.
 */
static int line_field(const char *line, const char *key, char *out, size_t cap) {
    char pat[48];
    const char *p;
    size_t o = 0;
    out[0] = 0;
    if (snprintf(pat, sizeof pat, "\"%s\":\"", key) >= (int)sizeof pat) return -1;
    p = strstr(line, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p && *p != '"' && o + 2 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            out[o++] = (*p == 'n' || *p == 'r' || *p == 't') ? ' ' : *p;
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return out[0] ? 0 : -1;
}

static int starts_ci(const char *s, const char *pfx) {
    size_t n = strlen(pfx), i;
    for (i = 0; i < n; i++) {
        if (!s[i]) return 0;
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)pfx[i])) return 0;
    }
    return 1;
}

/* An answer that carries no information. */
static int is_non_informative(const char *a) {
    static const char *pfx[] = {"abstain", "i do not", "i don't", "i'm not sure",
                                "im not sure", "no local", "(none)", "(teacher_miss)",
                                "you asked about", "sorry", NULL};
    int i;
    if (!a || strlen(a) < 24) return 1;
    while (*a == ' ' || *a == '[') {
        /* skip a leading [llm-live]/[lookup] tag but keep the body */
        if (*a == '[') {
            const char *c = strchr(a, ']');
            if (!c) break;
            a = c + 1;
            continue;
        }
        a++;
    }
    while (*a == ' ') a++;
    if (strlen(a) < 24) return 1;
    for (i = 0; pfx[i]; i++)
        if (starts_ci(a, pfx[i])) return 1;
    if (contains_ci(a, "no sealed skill")) return 1;
    if (contains_ci(a, "logged the miss")) return 1;
    return 0;
}

/* ANTI-COLLAPSE: CNET describing itself is our own output, not information. */
static int is_self_output(const char *a) {
    return contains_ci(a, "i'm marble") || contains_ci(a, "i am marble") ||
           contains_ci(a, "never self-cert") || contains_ci(a, "claimed_cert");
}

static int blocklisted(const char *q) {
    const char *bl = getenv("CNET_BLOCKLIST");
    char path[512], line[512];
    FILE *f;
    int hit = 0;
    if (bl && bl[0]) snprintf(path, sizeof path, "%s", bl);
    else snprintf(path, sizeof path, "config/promote_blocklist.txt");
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *bar;
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' || !line[0]) continue;
        bar = strchr(line, '|');
        if (!bar || strncmp(line, "substr", 6) != 0) continue;
        if (bar[1] && contains_ci(q, bar + 1)) { hit = 1; break; }
    }
    fclose(f);
    return hit;
}

/* Escape into a buffer with exactly the same rules jesc() writes with, so a
 * dedupe probe matches what is actually stored. */
static void jesc_buf(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; in && *in && o + 3 < cap; in++) {
        if (*in == '"' || *in == '\\') out[o++] = '\\';
        out[o++] = (*in == '\n' || *in == '\r') ? ' ' : *in;
    }
    out[o] = 0;
}

/* Already in the knowledge base? Compare on a prefix of the STORED (escaped)
 * form -- probing with the raw chunk missed every row containing a quote or a
 * backslash, so re-running the harvest kept re-adding the same notes. */
static int kb_has(const char *needle) {
    FILE *f = fopen(kb_path(), "r");
    char line[4096];
    char esc[512], probe[200];
    int hit = 0;
    if (!f) return 0;
    jesc_buf(needle, esc, sizeof esc);
    snprintf(probe, sizeof probe, "%.150s", esc);
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, probe)) { hit = 1; break; }
    }
    fclose(f);
    return hit;
}

static int harvest_miss_log(const char *path, int max_n) {
    FILE *in = fopen(path, "r");
    FILE *out;
    char *line = NULL;
    size_t cap = 0;
    int scanned = 0, kept = 0;
    int sk_abstain = 0, sk_self = 0, sk_block = 0, sk_dupe = 0;
    time_t now = time(NULL);
    if (!in) {
        fprintf(stderr, "cannot read %s\n", path);
        return -1;
    }
    ensure_parent(kb_path());
    out = fopen(kb_path(), "a");
    if (!out) { fclose(in); return -1; }
    while (getline(&line, &cap, in) > 0 && (max_n <= 0 || kept < max_n)) {
        char q[512], a[2048], chunk[2600];
        scanned++;
        if (line_field(line, "query", q, sizeof q) != 0) continue;
        if (line_field(line, "answer", a, sizeof a) != 0) continue;
        if (is_non_informative(a)) { sk_abstain++; continue; }
        if (is_self_output(a))     { sk_self++;    continue; }
        if (blocklisted(q))        { sk_block++;   continue; }
        snprintf(chunk, sizeof chunk, "Q: %.400s A: %.1600s", q, a);
        if (kb_has(chunk))         { sk_dupe++;    continue; }
        fprintf(out, "{\"ts\":%ld,\"source\":\"miss_log\",\"chunk\":\"", (long)now);
        jesc(out, chunk);
        fprintf(out, "\",\"claimed_cert\":0,\"kind\":\"miss_harvest\"}\n");
        fflush(out);
        kept++;
    }
    free(line);
    fclose(out);
    fclose(in);
    printf("MISS_HARVEST scanned=%d kept=%d skipped_abstain=%d skipped_self=%d "
           "skipped_blocked=%d skipped_dupe=%d claimed_cert=0 path=%s\n",
           scanned, kept, sk_abstain, sk_self, sk_block, sk_dupe, kb_path());
    printf("MISS_HARVEST_OK\n");
    return kept;
}

static int recall(const char *q, int maxn) {
    const char *path = kb_path();
    FILE *f = fopen(path, "r");
    char line[LINE];
    int n = 0;
    if (!f) {
        printf("KNOWLEDGE empty (%s)\n", path);
        printf("INGEST_RECALL_PASS\n");
        return 0;
    }
    while (fgets(line, sizeof line, f) && n < maxn) {
        const char *c = strstr(line, "\"chunk\":\"");
        if (!c) continue;
        c += 9;
        if (q && q[0] && !contains_ci(c, q) && !contains_ci(line, q)) continue;
        printf("HIT %s", line);
        n++;
    }
    fclose(f);
    printf("RECALL_HITS %d query=%s\n", n, q ? q : "*");
    printf("INGEST_RECALL_PASS\n");
    return 0;
}

static int slurp_file(const char *path, char **out) {
    FILE *f = fopen(path, "r");
    long sz;
    char *b;
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    if (sz < 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return -1;
    }
    rewind(f);
    b = (char *)malloc((size_t)sz + 1);
    if (!b) {
        fclose(f);
        return -1;
    }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return -1;
    }
    b[sz] = 0;
    fclose(f);
    *out = b;
    return 0;
}

int main(int argc, char **argv) {
    const char *src = "user";
    const char *text = NULL;
    const char *file = NULL;
    const char *rec = NULL;
    const char *misslog = NULL;
    int maxn = 0;
    int i, n;
    char *heap = NULL;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--source") && i + 1 < argc) src = argv[++i];
        else if (!strcmp(argv[i], "--text") && i + 1 < argc) text = argv[++i];
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "--recall") && i + 1 < argc) rec = argv[++i];
        else if (!strcmp(argv[i], "--from-miss-log") && i + 1 < argc) misslog = argv[++i];
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) maxn = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr,
                    "cnet_ingest_info --text|--file|--recall [--source NAME]\n"
                    "                 --from-miss-log PATH [--max N]\n");
            return 0;
        }
    }
    if (rec) return recall(rec, 8);
    if (misslog) return harvest_miss_log(misslog, maxn) < 0 ? 1 : 0;

    if (file) {
        if (slurp_file(file, &heap) != 0) {
            fprintf(stderr, "cannot read %s\n", file);
            return 1;
        }
        text = heap;
    }
    if (!text) {
        static char buf[65536];
        size_t g = fread(buf, 1, sizeof buf - 1, stdin);
        buf[g] = 0;
        text = buf;
    }
    n = ingest_buf(src, text);
    free(heap);
    if (n < 0) return 1;
    printf("INGEST source=%s chunks=%d path=%s claimed_cert=0\n", src, n, kb_path());
    printf("INGEST_INFO_PASS\n");
    return 0;
}
