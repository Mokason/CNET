/* cnet_aicimos_review_export — off-hot-path propose → AICIMOS packet JSONL.
 * Never CERT. Never writes packs/.lut/gold/cnetd. claimed_cert always 0.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cce/cce_campaign_provenance.h"

#define HEX 65
#define MAX_SEEN 4096
#define PATH_MAX_LOCAL 1024

static FILE *g_out;
static int g_n_packets;
static int g_n_abstain;
static int g_n_skip_dup;
static char g_seen[MAX_SEEN][HEX];
static int g_n_seen;
static char g_err[256];

static int seen_add(const char *hex) {
    int i;
    if (!hex || !hex[0]) return 1;
    for (i = 0; i < g_n_seen; i++)
        if (!strcmp(g_seen[i], hex)) return 1;
    if (g_n_seen >= MAX_SEEN) return 1;
    memcpy(g_seen[g_n_seen], hex, HEX);
    g_n_seen++;
    return 0;
}

static int is_probe_text(const char *s) {
    if (!s) return 0;
    if (strstr(s, "novel fact")) return 1;
    if (strstr(s, "mystic ooze")) return 1;
    if (strstr(s, "autonomous cycle probe")) return 1;
    if (strstr(s, "\"shortcircuit\":true")) return 1;
    if (strstr(s, "shortcircuit:true")) return 1;
    if (!strncmp(s, "zz", 2)) return 1;
    if (strstr(s, " zz") || strstr(s, "\"zz")) return 1;
    return 0;
}

static int json_has_empty_got(const char *s) {
    const char *p;
    if (!s) return 0;
    p = strstr(s, "\"got\"");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (!strncmp(p, "\"\"", 2)) return 1;
    return 0;
}

static const char *kind_from_propose(const char *body) {
    if (body && strstr(body, "\"ffi_convert\"")) return "ffi_convert";
    if (body && strstr(body, "\"kind\": \"capsule\"")) return "capsule";
    if (body && strstr(body, "\"kind\":\"capsule\"")) return "capsule";
    return "capsule";
}

static int json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    if (!in || !out || cap < 2) return -1;
    while (*in) {
        unsigned char c = (unsigned char)*in++;
        const char *rep = NULL;
        char tmp[8];
        size_t rl;
        if (c == '\\') rep = "\\\\";
        else if (c == '"') rep = "\\\"";
        else if (c == '\n') rep = "\\n";
        else if (c == '\r') rep = "\\r";
        else if (c == '\t') rep = "\\t";
        else if (c < 0x20) {
            snprintf(tmp, sizeof tmp, "\\u%04x", c);
            rep = tmp;
        }
        if (rep) {
            rl = strlen(rep);
            if (o + rl >= cap) return -1;
            memcpy(out + o, rep, rl);
            o += rl;
        } else {
            if (o + 1 >= cap) return -1;
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return 0;
}

static int emit_packet(const char *kind, const char *path, const char *sha,
                       const char *status) {
    char ep[PATH_MAX_LOCAL * 2];
    int abstain;
    if (!g_out || !kind || !path || !sha) return -1;
    if (seen_add(sha)) {
        g_n_skip_dup++;
        return 0;
    }
    if (json_escape(path, ep, sizeof ep) != 0) return -1;
    abstain = !strcmp(kind, "abstain") || (status && !strcmp(status, "abstain"));
    fprintf(g_out,
            "{\"schema_version\":1,\"kind\":\"%s\",\"source_path\":\"%s\","
            "\"source_sha256\":\"%s\",\"gold_id\":\"\","
            "\"auto_cert\":false,\"claimed_cert\":0,\"admitted\":false,"
            "\"observability_only\":true,\"requires_operator_review\":true,"
            "\"no_event_write\":true,\"no_memory_mutation\":true,"
            "\"no_skill_mutation\":true,\"status\":\"%s\","
            "\"law\":\"propose_only_never_self_cert\"}\n",
            abstain ? "abstain" : kind, ep, sha,
            abstain ? "abstain" : (status ? status : "pending_verify"));
    g_n_packets++;
    if (abstain) g_n_abstain++;
    return 0;
}

static int slurp(const char *path, char **out, size_t *n) {
    FILE *f;
    char *buf;
    long sz;
    *out = NULL;
    if (n) *n = 0;
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    sz = ftell(f);
    if (sz < 0 || sz > 8 * 1024 * 1024) { fclose(f); return -1; }
    rewind(f);
    buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    buf[sz] = 0;
    fclose(f);
    *out = buf;
    if (n) *n = (size_t)sz;
    return 0;
}

static int handle_propose(const char *path) {
    char sha[HEX];
    char *body = NULL;
    const char *kind;
    const char *st = "pending_verify";
    if (cce_sha256_file_hex(path, sha) != 0) return -1;
    if (slurp(path, &body, NULL) != 0) return -1;
    kind = kind_from_propose(body);
    if (is_probe_text(body) || json_has_empty_got(body)) {
        kind = "abstain";
        st = "abstain";
    }
    if (emit_packet(kind, path, sha, st) != 0) { free(body); return -1; }
    free(body);
    return 0;
}

static int handle_tsv(const char *path) {
    char sha[HEX];
    if (cce_sha256_file_hex(path, sha) != 0) return -1;
    return emit_packet("en_irregular", path, sha, "pending_verify");
}

static int handle_miss_log(const char *path) {
    FILE *f;
    char line[8192];
    int lineno = 0;
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char sha[HEX];
        char sp[PATH_MAX_LOCAL];
        size_t n = strlen(line);
        const char *kind = "miss_cluster";
        const char *st = "pending_verify";
        lineno++;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (!n) continue;
        if (cce_sha256_bytes_hex(line, n, sha) != 0) { fclose(f); return -1; }
        snprintf(sp, sizeof sp, "%s:%d", path, lineno);
        if (is_probe_text(line) || json_has_empty_got(line)) {
            kind = "abstain";
            st = "abstain";
        }
        if (emit_packet(kind, sp, sha, st) != 0) { fclose(f); return -1; }
    }
    fclose(f);
    return 0;
}

static int basename_is(const char *path, const char *want) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    return !strcmp(b, want);
}

static int handle_path(const char *path, int isdir) {
    if (isdir) return 0;
    if (basename_is(path, "PROPOSE.json")) return handle_propose(path);
    if (basename_is(path, "en_irregular_propose.tsv")) return handle_tsv(path);
    if (basename_is(path, "miss_log.jsonl")) return handle_miss_log(path);
    return 0;
}

static int nftw_cb(const char *fpath, const struct stat *sb, int typeflag,
                   struct FTW *ftwbuf) {
    (void)sb;
    (void)ftwbuf;
    if (typeflag == FTW_F) {
        if (handle_path(fpath, 0) != 0) {
            snprintf(g_err, sizeof g_err, "handle failed: %s", fpath);
            return 1;
        }
    }
    return 0;
}

static int export_in(const char *in_path) {
    struct stat st;
    if (stat(in_path, &st) != 0) {
        snprintf(g_err, sizeof g_err, "stat %s: %s", in_path, strerror(errno));
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (nftw(in_path, nftw_cb, 16, FTW_PHYS) != 0) return -1;
        return 0;
    }
    if (S_ISREG(st.st_mode)) return handle_path(in_path, 0);
    snprintf(g_err, sizeof g_err, "not a file or dir: %s", in_path);
    return -1;
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX_LOCAL];
    size_t i, n;
    n = strlen(path);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int mkdir_parents_of_file(const char *file) {
    char tmp[PATH_MAX_LOCAL];
    char *slash;
    snprintf(tmp, sizeof tmp, "%s", file);
    slash = strrchr(tmp, '/');
    if (!slash) return 0;
    *slash = 0;
    return mkdir_p(tmp);
}

static int write_file(const char *path, const char *body) {
    FILE *f;
    if (mkdir_parents_of_file(path) != 0) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    fputs(body, f);
    fclose(f);
    return 0;
}

static int run_selftest(void) {
    char dir[] = "/tmp/cnet-aicimos-export-XXXXXX";
    char propose[PATH_MAX_LOCAL], miss[PATH_MAX_LOCAL], tsv[PATH_MAX_LOCAL];
    char outp[PATH_MAX_LOCAL], lut[PATH_MAX_LOCAL], gold[PATH_MAX_LOCAL];
    char sha_file[HEX];
    FILE *f;
    char line[4096];
    int saw_ffi = 0, saw_abs = 0, saw_en = 0, bad_cert = 0, invented = 0;

    if (!mkdtemp(dir)) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    snprintf(propose, sizeof propose, "%s/inbox/ffi-1/PROPOSE.json", dir);
    snprintf(miss, sizeof miss, "%s/var/miss_log.jsonl", dir);
    snprintf(tsv, sizeof tsv, "%s/en_irregular_propose.tsv", dir);
    snprintf(outp, sizeof outp, "%s/out/packets.jsonl", dir);
    snprintf(lut, sizeof lut, "%s/q1_fake.lut", dir);
    snprintf(gold, sizeof gold, "%s/gold", dir);
    if (write_file(propose,
                   "{\n  \"kind\": \"ffi_convert\",\n  \"site\": \"host.embed.0\",\n"
                   "  \"sent\": \"go\",\n  \"got\": \"went\",\n"
                   "  \"auto_cert\": false,\n  \"admitted\": false\n}\n") != 0)
        return 1;
    if (write_file(miss,
                   "{\"q\":\"autonomous cycle probe novel fact beta-nine\"}\n"
                   "{\"q\":\"past tense of fly\"}\n") != 0)
        return 1;
    if (write_file(tsv, "cling\tclung\t\n") != 0) return 1;
    if (mkdir_parents_of_file(outp) != 0) return 1;
    g_out = fopen(outp, "w");
    if (!g_out) return 1;
    if (export_in(dir) != 0) {
        fprintf(stderr, "export: %s\n", g_err[0] ? g_err : "fail");
        fclose(g_out);
        return 1;
    }
    fflush(g_out);
    fclose(g_out);
    g_out = NULL;

    if (cce_sha256_file_hex(propose, sha_file) != 0) return 1;
    f = fopen(outp, "r");
    if (!f) {
        fprintf(stderr, "missing packets %s\n", outp);
        return 1;
    }
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "\"claimed_cert\":1") || strstr(line, "\"claimed_cert\": 1"))
            bad_cert = 1;
        if (strstr(line, "\"kind\":\"ffi_convert\"") && strstr(line, sha_file))
            saw_ffi = 1;
        if (strstr(line, "\"kind\":\"en_irregular\"")) saw_en = 1;
        if (strstr(line, "\"kind\":\"abstain\"")) {
            saw_abs = 1;
            if (strstr(line, "\"got\"")) invented = 1;
        }
    }
    fclose(f);

    if (access(lut, F_OK) == 0) {
        fprintf(stderr, "lut created — forbidden\n");
        return 1;
    }
    if (access(gold, F_OK) == 0) {
        fprintf(stderr, "gold dir created — forbidden\n");
        return 1;
    }
    if (bad_cert || !saw_ffi || !saw_abs || !saw_en || invented) {
        fprintf(stderr,
                "selftest fail ffi=%d abs=%d en=%d cert=%d invented=%d packets=%d\n",
                saw_ffi, saw_abs, saw_en, bad_cert, invented, g_n_packets);
        return 1;
    }
    printf("AICIMOS_REVIEW_EXPORT_PASS packets=%d abstain=%d dup=%d\n",
           g_n_packets, g_n_abstain, g_n_skip_dup);
    printf("claimed_cert=0 observability_only=true auto_cert=false\n");
    printf("source_sha256_ffi=%s\n", sha_file);
    return 0;
}

static void usage(void) {
    fprintf(stderr,
            "usage: cnet_aicimos_review_export --in PATH --out PACKETS.jsonl\n"
            "       cnet_aicimos_review_export --test\n"
            "Off-hot-path. Never CERT. claimed_cert=0.\n");
}

int main(int argc, char **argv) {
    const char *in_path = NULL;
    const char *out_path = NULL;
    int test = 0;
    int i;
    char parent[PATH_MAX_LOCAL];
    char *slash;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--in") && i + 1 < argc) in_path = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--test")) test = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }

    if (test)
        return run_selftest();

    if (!in_path || !out_path) {
        usage();
        return 2;
    }
    snprintf(parent, sizeof parent, "%s", out_path);
    slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = 0;
        if (mkdir_p(parent) != 0) {
            fprintf(stderr, "mkdir %s: %s\n", parent, strerror(errno));
            return 1;
        }
    }
    g_out = fopen(out_path, "w");
    if (!g_out) {
        fprintf(stderr, "open %s: %s\n", out_path, strerror(errno));
        return 1;
    }
    if (export_in(in_path) != 0) {
        fprintf(stderr, "export failed: %s\n", g_err[0] ? g_err : "unknown");
        fclose(g_out);
        return 1;
    }
    fclose(g_out);
    printf("AICIMOS_REVIEW_EXPORT_PASS packets=%d abstain=%d dup=%d\n",
           g_n_packets, g_n_abstain, g_n_skip_dup);
    printf("claimed_cert=0 observability_only=true out=%s\n", out_path);
    return 0;
}
