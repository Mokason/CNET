#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cnet_capsule.h"
#include "cnet_capsule_evidence.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int atom(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n || n >= CNB_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
            (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return 0;
    return 1;
}
static int fault(const char *point) {
#ifdef CNET_SOURCE_CAPSULE_TESTING
    const char *selected = getenv("CNET_SOURCE_FAIL_SYNC");
    if (selected && !strcmp(selected, point)) { errno = EIO; return -1; }
#else
    (void)point;
#endif
    return 0;
}
static int sync_output(int fd, const char *point) {
    return fault(point) || fsync(fd) ? -1 : 0;
}
/* Only an existing owner-controlled parent is accepted. All path components
 * are reopened with nofollow; no shell, dot traversal, collision or overwrite. */
static int output_parent(const char *path, char leaf[256]) {
    char copy[4096];
    if (!path || path[0] != '/' || strlen(path) >= sizeof copy) return -1;
    strcpy(copy, path + 1);
    char *last = strrchr(copy, '/');
    if (!last || !last[1] || strlen(last + 1) >= 256) return -1;
    strcpy(leaf, last + 1); *last = 0;
    if (strlen(leaf) > 96 || leaf[0] == '.') return -1;
    for (const char *s = leaf; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
            (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' || *s == '.')) return -1;
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    char *part = copy;
    while (part && *part) {
        char *slash = strchr(part, '/'); if (slash) *slash = 0;
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) { close(fd); return -1; }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd); fd = next;
        if (fd < 0) return -1;
        part = slash ? slash + 1 : NULL;
        if (part && !*part) { close(fd); return -1; }
    }
    struct stat st;
    if (fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 0022)) { close(fd); return -1; }
    return fd;
}
static int build(const char *source, const char *output, const char *unit) {
    void *asset = NULL, *roundtrip = NULL; size_t asset_len = 0, roundtrip_len = 0;
    unsigned roundtrip_schema = 0;
    CnetBase base, imported; HybridAi coverage, restored;
    BinaryTransformNetwork btn = {0}, imported_btn = {0}; Contract contract = {0}, imported_contract = {0};
    CnetCapsuleEvidence *evidence = NULL;
    cnb_init(&base); cnb_init(&imported); hybrid_ai_init(&coverage); hybrid_ai_init(&restored);
    int rc = 1, parent = -1, dir = -1, created = 0, complete = 0;
    char error[160] = "invalid_argument", leaf[256], pinned[320];
    double rows[CNET_CAPSULE_EVIDENCE_FACTS * 3];
    if (!atom(unit)) goto done;
    parent = output_parent(output, leaf);
    if (parent < 0) { strcpy(error, "output_parent_ownership_or_path"); goto done; }
    if (cnet_capsule_evidence_acquire(source, &asset, &asset_len, error, sizeof error)) goto done;
    strcpy(error, "finite_compile_or_certification");
    Port pin = {PORT_BINARY_MSB, 3, 1, CNET_CAPSULE_EVIDENCE_INPUT};
    Port pout = {PORT_BINARY_MSB, 3, 1, CNET_CAPSULE_EVIDENCE_OUTPUT};
    const unsigned n = CNET_CAPSULE_EVIDENCE_FACTS;
    if (btn_init(&btn, 3, 3, n, n, .1, 7) || btn_set_ports(&btn, pin, pout)) goto done;
    /* Existing finite-domain compiler construction (capsule_core teach): one
     * exact binary detector per evidence label; coverage excludes 5..7. */
    for (unsigned i = 0; i < n; i++) {
        unsigned ones = 0;
        for (unsigned b = 0; b < 3; b++) {
            unsigned bit = (i >> (2 - b)) & 1u; ones += bit;
            rows[i * 3 + b] = bit;
            btn.input_hidden[i * 3 + b] = bit ? 32.0 : -32.0;
            btn.hidden_output_weights[b * n + i] = bit ? 32.0 : 0.0;
        }
        btn.hidden_bias[i] = 32.0 * (.5 - ones);
    }
    for (unsigned b = 0; b < 3; b++) btn.output_bias[b] = -16.0;
    CertifyReport cert;
    if (contract_init_borrowed(&contract, unit, &btn, rows, rows, n) ||
        btn_certify_robust(&btn, &contract, .05, &cert) || cnb_add_unit(&base, &btn, &contract, NULL) ||
        cnb_add_oracle_desc(&base, "literal_line_tool", "verified_tool", pin, pout) ||
        cnb_set_unit_provenance(&base, unit, "literal_line_tool") ||
        hybrid_coverage_record(&coverage, pin, pout, unit, rows, rows, n, 3, 3)) goto done;
    evidence = cnet_capsule_evidence_parse(asset, asset_len, CNET_CAPSULE_EVIDENCE_SCHEMA,
        &contract, &coverage.coverage[0], error, sizeof error);
    if (!evidence || cnet_capsule_evidence_fresh(evidence, source, error, sizeof error)) goto done;
    if (mkdirat(parent, leaf, 0700)) { strcpy(error, "output_collision_or_create"); goto done; }
    created = 1;
    dir = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) { strcpy(error, "output_directory_open"); goto done; }
    /* Final '.' lets the exporter's nofollow parent-sync open a directory;
     * the only symlink traversed is our own pinned proc descriptor. */
    snprintf(pinned, sizeof pinned, "/proc/self/fd/%d/.", dir);
    if (fault("before_export")) { strcpy(error, "before_export_sync_injected"); goto done; }
    CnetCapsuleReport report;
    if (cnet_capsule_export_asset(&base, &coverage, unit, pinned, asset, asset_len, CNET_CAPSULE_EVIDENCE_SCHEMA, &report)) {
        snprintf(error, sizeof error, "export: %.140s", report.reject_reason); goto done;
    }
    strcpy(error, "capsule_roundtrip_or_certification");
    if (cnet_capsule_import_asset(&imported, &restored, pinned, &roundtrip, &roundtrip_len, &roundtrip_schema, &report) ||
        roundtrip_len != asset_len || roundtrip_schema != CNET_CAPSULE_EVIDENCE_SCHEMA || memcmp(asset, roundtrip, asset_len) ||
        cnb_get_unit(&imported, unit, &imported_btn, &imported_contract) ||
        btn_certify_robust(&imported_btn, &imported_contract, .05, &cert)) goto done;
    cnet_capsule_evidence_close(evidence);
    evidence = cnet_capsule_evidence_parse(roundtrip, roundtrip_len, roundtrip_schema, &imported_contract,
        &restored.coverage[0], error, sizeof error);
    if (!evidence || cnet_capsule_evidence_fresh(evidence, source, error, sizeof error)) goto done;
    complete = 1;
    if (sync_output(dir, "directory_commit") || sync_output(parent, "parent_commit")) {
        fprintf(stderr, "SOURCE_CAPSULE_COMMIT_UNCERTAIN retained=1 out=%s\n", output);
        rc = 3; goto done;
    }
    char identity[65];
    if (cnet_capsule_evidence_identity(evidence, identity)) goto done;
    printf("SOURCE_CAPSULE_PASS unit=%s facts=%u source=verified_tool receipt=literal_line_check asset_sha256=%s margin=%.6f pending_activation=1\n",
        unit, n, identity, cert.min_margin);
    rc = 0;
done:
    if (created && !complete) {
        if (dir >= 0) {
            unlinkat(dir, "manifest.cknow", 0); unlinkat(dir, "unit.cnb", 0); unlinkat(dir, CNET_CAPSULE_ASSET_FILE, 0);
        }
        unlinkat(parent, leaf, AT_REMOVEDIR);
    }
    if (dir >= 0) close(dir);
    if (parent >= 0) close(parent);
    if (rc == 1) fprintf(stderr, "SOURCE_CAPSULE_REFUSED %s\n", error);
    cnet_capsule_evidence_close(evidence); free(asset); free(roundtrip);
    contract_free(&contract); contract_free(&imported_contract); btn_free(&btn); btn_free(&imported_btn);
    cnb_free(&base); cnb_free(&imported); hybrid_ai_free(&coverage); hybrid_ai_free(&restored);
    return rc;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "list")) {
        for (unsigned i = 0; i < CNET_CAPSULE_EVIDENCE_FACTS; i++) puts(cnet_capsule_evidence_fact_name(i));
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "request")) {
        char request[128];
        if (cnet_capsule_evidence_request(argv[2], request, sizeof request)) return 1;
        puts(request); return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "build")) return build(argv[2], argv[3], argv[4]);
    fprintf(stderr, "usage: %s list | request FACT | build OWNER_SOURCE_ROOT NEW_CAPSULE_DIRECTORY UNIT\n", argv[0]);
    return 1;
}
