#include "../include/cce/cce_campaign_provenance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *name) {
    if (ok) printf("  PASS: %s\n", name);
    else { printf("  FAIL: %s\n", name); failures++; }
}

static int put(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    size_t n = strlen(text);
    if (!f) return -1;
    if (fwrite(text, 1, n, f) != n) { fclose(f); return -1; }
    return fclose(f) == 0 ? 0 : -1;
}

static int write_manifest(const char *path, const char *exe,
                          const char *model, const char *window,
                          const char *golden, const char *base,
                          const char *rev, int dirty, int omit_exe_hash) {
    char eh[65], mh[65], wh[65], gh[65], bh[65];
    FILE *f;
    if (cce_sha256_file_hex(exe, eh) != 0 ||
        cce_sha256_file_hex(model, mh) != 0 ||
        cce_sha256_file_hex(window, wh) != 0 ||
        cce_sha256_file_hex(golden, gh) != 0 ||
        cce_sha256_file_hex(base, bh) != 0) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "{\n");
    fprintf(f, "  \"provenance_version\": 1,\n");
    fprintf(f, "  \"build_rev\": \"%s\",\n", rev);
    fprintf(f, "  \"source_dirty\": %d,\n", dirty);
    if (!omit_exe_hash)
        fprintf(f, "  \"executable_sha256\": \"%s\",\n", eh);
    fprintf(f, "  \"model\": \"%s\",\n", model);
    fprintf(f, "  \"model_sha256\": \"%s\",\n", mh);
    fprintf(f, "  \"window_source\": \"%s\",\n", window);
    fprintf(f, "  \"window_sha256\": \"%s\",\n", wh);
    fprintf(f, "  \"oracle_golden\": \"%s\",\n", golden);
    fprintf(f, "  \"oracle_golden_sha256\": \"%s\",\n", gh);
    fprintf(f, "  \"base\": \"%s\",\n", base);
    fprintf(f, "  \"base_sha256\": \"%s\"\n", bh);
    fprintf(f, "}\n");
    return fclose(f) == 0 ? 0 : -1;
}

int main(int argc, char **argv) {
    char root[160], manifest[192], exe[192], model[192], window[192];
    char golden[192], base[192], hash[65], err[256];
    int rc;
    if (argc == 5) {
        rc = cce_campaign_provenance_verify(argv[1], argv[2], argv[3],
                                            atoi(argv[4]), err, sizeof err);
        if (rc != 0) {
            fprintf(stderr, "CAMPAIGN_ARTIFACTS_FAIL: %s\n", err);
            return 1;
        }
        printf("CAMPAIGN_ARTIFACTS_PASS\n");
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [manifest executable build_rev dirty]\n",
                argv[0]);
        return 2;
    }
    snprintf(root, sizeof root, "/tmp/cnet-provenance-%ld", (long)getpid());
    snprintf(manifest, sizeof manifest, "%s.json", root);
    snprintf(exe, sizeof exe, "%s.exe", root);
    snprintf(model, sizeof model, "%s.model", root);
    snprintf(window, sizeof window, "%s.window", root);
    snprintf(golden, sizeof golden, "%s.golden", root);
    snprintf(base, sizeof base, "%s.base", root);

    check(put(exe, "abc") == 0 &&
          cce_sha256_file_hex(exe, hash) == 0 &&
          strcmp(hash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
          "SHA-256 matches the standard abc vector");
    check(put(model, "model-a") == 0 && put(window, "1\n2\n3\n") == 0 &&
          put(golden, "1 2 -> 3\n") == 0 && put(base, "base-a") == 0,
          "fixture artifacts written");
    check(write_manifest(manifest, exe, model, window, golden, base,
                         "abc1234", 1, 0) == 0,
          "complete provenance manifest written");

    rc = cce_campaign_provenance_verify(manifest, exe, "abc1234", 1,
                                        err, sizeof err);
    check(rc == 0, "matching source, binary, model, window, golden and base pass");

    put(model, "model-b");
    rc = cce_campaign_provenance_verify(manifest, exe, "abc1234", 1,
                                        err, sizeof err);
    check(rc != 0 && strstr(err, "model") != NULL,
          "model content mismatch is refused");
    put(model, "model-a");

    put(window, "1\n3\n2\n");
    rc = cce_campaign_provenance_verify(manifest, exe, "abc1234", 1,
                                        err, sizeof err);
    check(rc != 0 && strstr(err, "window") != NULL,
          "ordered window mismatch is refused");
    put(window, "1\n2\n3\n");

    put(base, "base-b");
    rc = cce_campaign_provenance_verify(manifest, exe, "abc1234", 1,
                                        err, sizeof err);
    check(rc != 0 && strstr(err, "base") != NULL,
          "resume base mismatch is refused");
    put(base, "base-a");

    rc = cce_campaign_provenance_verify(manifest, exe, "def5678", 1,
                                        err, sizeof err);
    check(rc != 0 && strstr(err, "build_rev") != NULL,
          "source revision mismatch is refused");
    rc = cce_campaign_provenance_verify(manifest, exe, "abc1234", 0,
                                        err, sizeof err);
    check(rc != 0 && strstr(err, "source_dirty") != NULL,
          "source dirty-state mismatch is refused");

    check(write_manifest(manifest, exe, model, window, golden, base,
                         "abc1234", 1, 1) == 0,
          "incomplete manifest fixture written");
    rc = cce_campaign_provenance_verify(manifest, exe, "abc1234", 1,
                                        err, sizeof err);
    check(rc != 0 && strstr(err, "executable_sha256") != NULL,
          "missing executable fingerprint is refused");

    remove(manifest); remove(exe); remove(model); remove(window);
    remove(golden); remove(base);
    if (failures) {
        printf("CAMPAIGN_PROVENANCE_FAIL (%d failures)\n", failures);
        return 1;
    }
    printf("CAMPAIGN_PROVENANCE_PASS (11 checks)\n");
    return 0;
}
