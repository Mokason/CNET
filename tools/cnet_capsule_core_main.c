#include "cnet_capsule_core.h"
#include "cnet_capsule.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

static int atom(const char *s, size_t max) {
    size_t n = strlen(s);
    if (!n || n >= max) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_') return 0;
    return 1;
}
static int uint_value(const char *s, unsigned max, unsigned *v) {
    if (!s[0]) return -1;
    for (size_t i = 0; s[i]; i++) if (!isdigit((unsigned char)s[i])) return -1;
    errno = 0; char *end; unsigned long n = strtoul(s, &end, 10);
    if (errno || *end || n > max) return -1;
    *v = (unsigned)n; return 0;
}
static int same_capsule_files(const char *a, const char *b) {
    const char *files[] = {"unit.cnb", "manifest.cknow"};
    for (size_t i = 0; i < 2; i++) {
        char pa[1300], pb[1300]; unsigned char ba[4096], bb[4096];
        if (snprintf(pa, sizeof pa, "%s/%s", a, files[i]) >= (int)sizeof pa ||
            snprintf(pb, sizeof pb, "%s/%s", b, files[i]) >= (int)sizeof pb) return 0;
        FILE *fa = fopen(pa, "rb"), *fb = fopen(pb, "rb");
        if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
        int same = 1;
        for (;;) {
            size_t na = fread(ba, 1, sizeof ba, fa), nb = fread(bb, 1, sizeof bb, fb);
            if (na != nb || memcmp(ba, bb, na)) { same = 0; break; }
            if (!na) break;
        }
        if (ferror(fa) || ferror(fb)) same = 0;
        fclose(fa); fclose(fb);
        if (!same) return 0;
    }
    return 1;
}
static int evaluate_candidate(const char *root, CnetCapsuleCore *candidate,
                              const char *path, int reused) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char error[160], line[256], requests[256][128];
    CnetCapsuleCore *before = cnet_capsule_core_open(root, error, sizeof error);
    if (!before) { fclose(f); return -1; }
    unsigned n = 0, prior = 0, after = 0, wrong = 0;
    int failed = 0;
    while (fgets(line, sizeof line, f)) {
        char in[32], out[32], x[32], y[32], extra;
        unsigned input, expected;
        if (n == 256 || sscanf(line, "%31s %31s %31s %31s %c", in, out, x, y, &extra) != 4 ||
            !atom(in, 32) || !atom(out, 32) || uint_value(x, 65535, &input) || uint_value(y, 65535, &expected)) {
            failed = 1; break;
        }
        snprintf(requests[n], sizeof requests[n], "capsule %s %s %u", in, out, input);
        for (unsigned i = 0; i < n; i++) if (!strcmp(requests[i], requests[n])) failed = 1;
        if (failed) break;
        CnetCapsuleCoreReply r;
        if (!cnet_capsule_core_ask(before, requests[n], &r) && r.verified && r.value == expected) prior++;
        if (!cnet_capsule_core_ask(candidate, requests[n], &r) && r.verified) {
            if (r.value == expected) after++; else wrong++;
        }
        n++;
    }
    failed |= ferror(f); fclose(f); cnet_capsule_core_close(before);
    int pass = !failed && n && after == n && !wrong && (reused || after > prior);
    printf("CAPSULE_EVAL_%s cases=%u before_correct=%u after_correct=%u wrong_certified=%u teacher_calls=0\n",
           pass ? "PASS" : "REFUSED", n, prior, after, wrong);
    return pass ? 0 : -1;
}
static int teach(int argc, char **v) {
    unsigned ib, ob, n = 0;
    int rc = 1, published = 0, lock_fd = -1, reused = 0;
    char line[256], final[1200], stage[1200] = "";
    double input[256 * 16], target[256 * 16];
    unsigned char seen[65536] = {0};
    BinaryTransformNetwork btn = {0}; Contract ct = {0};
    CnetBase base; HybridAi coverage; CnetCapsuleReport rep;
    cnb_init(&base); hybrid_ai_init(&coverage);
    if (argc != 10 || !atom(v[3], 64) || !atom(v[4], 32) || !atom(v[5], 32) ||
        uint_value(v[6], 16, &ib) || !ib || uint_value(v[7], 16, &ob) || !ob ||
        (strcmp(v[8], "user_correction") && strcmp(v[8], "verified_tool"))) goto done;
    FILE *f = fopen(v[9], "r");
    if (!f) goto done;
    int invalid = 0;
    while (fgets(line, sizeof line, f)) {
        char a[32], b[32], extra; unsigned x, y;
        if (n == 256 || sscanf(line, "%31s %31s %c", a, b, &extra) != 2 ||
            uint_value(a, (1u << ib) - 1, &x) || uint_value(b, (1u << ob) - 1, &y) || seen[x]) {
            invalid = 1; break;
        }
        seen[x] = 1;
        for (unsigned j = 0; j < ib; j++) input[n * ib + j] = (x >> (ib - 1 - j)) & 1u;
        for (unsigned j = 0; j < ob; j++) target[n * ob + j] = (y >> (ob - 1 - j)) & 1u;
        n++;
    }
    invalid |= ferror(f); fclose(f);
    if (invalid || !n) goto done;
    Port pin = {PORT_BINARY_MSB, ib, 1, ""}, pout = {PORT_BINARY_MSB, ob, 1, ""};
    snprintf(pin.tag, sizeof pin.tag, "%s", v[4]); snprintf(pout.tag, sizeof pout.tag, "%s", v[5]);
    const char *method = "gradient_fit";
    if (btn_init(&btn, ib, ob, 16, 128, .5, 7) || btn_set_ports(&btn, pin, pout)) goto done;
    /* These rows define a finite certified domain, not a held-out population.
     * Small fits may compress it. A finite decoder is the bounded fallback. */
    double loss = n <= 16 ? btn_train_dynamic_spec(&btn, input, target, n, 30000, 200, 1e-5, 1e-7) : BTN_TRAIN_LOSS_FAILED;
    int fitted = btn_train_loss_is_success(loss) && !btn_train(&btn, input, target, n, 4000);
    CertifyReport cert;
    if (contract_init_borrowed(&ct, v[3], &btn, input, target, n)) goto done;
    if (!fitted || btn_certify_robust(&btn, &ct, .05, &cert)) {
        contract_free(&ct); btn_free(&btn);
        if (btn_init(&btn, ib, ob, n, n, .1, 7) || btn_set_ports(&btn, pin, pout)) goto done;
        /* Hidden row j detects exact binary equality: its preactivation is
         * 32*(0.5-HammingDistance). At <=256 rows, aggregate off-row leakage
         * stays below 256*exp(-16). Each output ORs the matching positive rows.
         * Outside the supplied rows the attached coverage guard still refuses. */
        for (unsigned j = 0; j < n; j++) {
            unsigned ones = 0;
            for (unsigned k = 0; k < ib; k++) {
                unsigned bit = (unsigned)input[j * ib + k]; ones += bit;
                btn.input_hidden[j * ib + k] = bit ? 32.0 : -32.0;
            }
            btn.hidden_bias[j] = 32.0 * (.5 - ones);
            for (unsigned k = 0; k < ob; k++)
                btn.hidden_output_weights[k * n + j] = target[j * ob + k] ? 32.0 : 0.0;
        }
        for (unsigned k = 0; k < ob; k++) btn.output_bias[k] = -16.0;
        if (contract_init_borrowed(&ct, v[3], &btn, input, target, n)) goto done;
        method = "finite_domain_compile";
    }
    if (btn_certify_robust(&btn, &ct, .05, &cert)) {
        fprintf(stderr, "CAPSULE_TEACH_REFUSED certification rows=%u passed=%zu failed=%zu minimum_margin=%.9f required_margin=0.05\n", n, cert.passed, cert.failed, cert.min_margin);
        goto done;
    }
    if (cnb_add_unit(&base, &btn, &ct, NULL) ||
        cnb_add_oracle_desc(&base, v[8], v[8], pin, pout) ||
        cnb_set_unit_provenance(&base, v[3], v[8]) ||
        hybrid_coverage_record(&coverage, pin, pout, v[3], input, target, n, ib, ob)) goto done;
    struct stat st;
    if (mkdir(v[2], 0700) && errno != EEXIST) goto done;
    if (lstat(v[2], &st) || !S_ISDIR(st.st_mode)) goto done;
    char lock_path[1200];
    if (snprintf(lock_path, sizeof lock_path, "%s/.publish.lock", v[2]) >= (int)sizeof lock_path) goto done;
    lock_fd = open(lock_path, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX)) goto done;
    int k = snprintf(final, sizeof final, "%s/%s", v[2], v[3]);
    if (k < 0 || (size_t)k >= sizeof final) goto done;
    if (!lstat(final, &st)) { if (!S_ISDIR(st.st_mode)) goto done; reused = 1; }
    else if (errno != ENOENT) goto done;
    k = snprintf(stage, sizeof stage, "%s/.pending-XXXXXX", v[2]);
    if (k < 0 || (size_t)k >= sizeof stage || !mkdtemp(stage)) { stage[0] = 0; goto done; }
    if (cnet_capsule_export(&base, &coverage, v[3], stage, &rep)) goto done;
    CnetBase probe; HybridAi guard;
    cnb_init(&probe); hybrid_ai_init(&guard);
    int valid = cnet_capsule_import(&probe, &guard, stage, &rep) == 0;
    cnb_free(&probe); hybrid_ai_free(&guard);
    if (!valid) goto done;
    char error[160];
    if (reused && !same_capsule_files(stage, final)) goto done;
    CnetCapsuleCore *candidate = reused ? cnet_capsule_core_open(v[2], error, sizeof error) :
        cnet_capsule_core_open_candidate(v[2], stage, error, sizeof error);
    if (!candidate) { fprintf(stderr, "CAPSULE_TEACH_REFUSED %s\n", error); goto done; }
    CnetCapsuleCore *previous = cnet_capsule_core_open(v[2], error, sizeof error);
    size_t obligations = 0;
    int regression = !previous || cnet_capsule_core_validate_growth(previous, candidate, &obligations);
    cnet_capsule_core_close(previous);
    printf("CAPSULE_HISTORY_%s label_obligations=%zu\n", regression ? "REFUSED" : "PASS", obligations);
    if (regression) { cnet_capsule_core_close(candidate); goto done; }
    /* A retry is valid only if importing the complete proposed artifact into
     * the existing view succeeds with the same sealed unit identity. */
    const char *evaluation = getenv("CNET_CAPSULE_EVAL_FILE");
    int evaluated = !evaluation || !evaluation[0] || !evaluate_candidate(v[2], candidate, evaluation, reused);
    cnet_capsule_core_close(candidate);
    if (!evaluated) goto done;
    /* Linux atomic no-clobber publication; never replace an installed unit. */
    if (!reused && renameat2(AT_FDCWD, stage, AT_FDCWD, final, RENAME_NOREPLACE)) goto done;
    published = !reused;
    /* Also repair an identical retry after a prior rename succeeded but its
     * sync failed. Keep the certified artifact on post-rename failure. */
    if (cnb_sync_parent(final) || cnb_sync_parent(v[2])) goto done;
    rc = 0;
    printf("CAPSULE_TEACH_PASS unit=%s rows=%u source=%s digest=%llu scope=%s margin=%.6f reused=%d method=%s\n",
           v[3], n, v[8], rep.behavior_digest, rep.scope, cert.min_margin, reused, method);
done:
    if (stage[0] && !published) {
        char file[1300];
        snprintf(file, sizeof file, "%s/unit.cnb", stage); unlink(file);
        snprintf(file, sizeof file, "%s/manifest.cknow", stage); unlink(file);
        rmdir(stage);
    }
    if (rc) fprintf(stderr, "CAPSULE_TEACH_REFUSED invalid evidence, failed certification, or publication conflict\n");
    contract_free(&ct); btn_free(&btn); cnb_free(&base); hybrid_ai_free(&coverage);
    if (lock_fd >= 0) close(lock_fd);
    return rc;
}
static int inspect(const char *path) {
    CnetBase b; cnb_init(&b);
    if (cnb_load(&b, path)) return 1;
    for (size_t i = 0; i < b.unit_count; i++) {
        if (!hybrid_unit_is_mined(b.units[i].name)) continue;
        BinaryTransformNetwork btn = {0}; Contract ct = {0};
        if (cnb_get_unit(&b, b.units[i].name, &btn, &ct)) { cnb_free(&b); return 1; }
        printf("unit=%s provenance=%s rows=%zu input=%s/%zu/%d output=%s/%zu/%d\n",
            b.units[i].name, b.units[i].provenance, ct.exemplar_count,
            btn.input_ports[0].tag, btn.input_count, btn.input_ports[0].family,
            btn.output_ports[0].tag, btn.output_count, btn.output_ports[0].family);
        contract_free(&ct); btn_free(&btn);
    }
    cnb_free(&b); return 0;
}
int main(int argc, char **argv) {
    if(argc==5&&!strcmp(argv[1],"reuse")) {
        char digest[65];size_t bytes=0;CnetCapsuleCoreReply reply;
        if(cnet_capsule_core_reuse(argv[2],argv[3],argv[4],digest,&bytes,&reply)) {
            fprintf(stderr,"REUSE_REFUSED reason=%s\n",reply.reason);return 3;
        }
        if(printf("REUSE verified=1 snapshot_sha256=%s bytes=%zu value=%u hops=%zu units=%s\n",
                  digest,bytes,reply.value,reply.hops,reply.units)<0||fflush(stdout))return 3;
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "teach")) return teach(argc, argv);
    if (argc == 3 && !strcmp(argv[1], "inspect")) return inspect(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "ask")) {
        char error[160]; CnetCapsuleCoreReply r;
        CnetCapsuleCore *c = cnet_capsule_core_open(argv[2], error, sizeof error);
        if (!c) { printf("ABSTAIN verified=0 reason=%s\n", error); return 3; }
        int rc = cnet_capsule_core_ask(c, argv[3], &r);
        if (rc) printf("ABSTAIN verified=0 reason=%s\n", r.reason);
        else printf("LOCAL verified=1 value=%u hops=%zu units=%s\n", r.value, r.hops, r.units);
        cnet_capsule_core_close(c); return rc ? 3 : 0;
    }
    fprintf(stderr, "usage: %s ask ROOT 'capsule INPUT_TAG OUTPUT_TAG N'\n"
        "       %s teach ROOT UNIT INPUT_TAG OUTPUT_TAG INPUT_BITS OUTPUT_BITS user_correction|verified_tool ROWS_TSV\n"
        "       %s inspect BASE\n"
        "       %s reuse ABS_SOURCE ABS_DESTINATION 'capsule INPUT_TAG OUTPUT_TAG N'\n", argv[0], argv[0], argv[0], argv[0]);
    return 2;
}
