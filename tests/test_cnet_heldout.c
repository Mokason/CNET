/* Unit gate for the held-out fixture reader.
 *
 * The capability certificate is only as trustworthy as this reader: if it can
 * be made to accept a fixture it did not really consume, or to report a digest
 * that is not the digest of the bytes it parsed, every downstream causality
 * claim is decorative again. So the negatives here are the point — a fixture
 * that names another capability, a case with no id, a duplicate id, a key that
 * is absent or the wrong type, a case nobody read, and a case whose verdict
 * failed must each make cnet_heldout_finish() report failure.
 */
#include "../include/cnet_heldout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static int checks;

static void check(int condition, const char *message) {
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static char dir_template[] = "/tmp/cnet-heldout-XXXXXX";
static char *scratch_dir;

static void write_fixture(const char *leaf, const char *body, char *out,
                          size_t cap) {
    FILE *fp;
    snprintf(out, cap, "%s/%s", scratch_dir, leaf);
    fp = fopen(out, "wb");
    if (!fp) {
        fprintf(stderr, "FAIL: cannot write %s\n", out);
        failures++;
        return;
    }
    fputs(body, fp);
    fclose(fp);
}

static void set_fixture(const char *path) {
    if (path) setenv("CNET_HELD_OUT_FIXTURE", path, 1);
    else unsetenv("CNET_HELD_OUT_FIXTURE");
}

/* SHA-256 is inlined in the reader; prove it against the standard vectors by
   hashing files with known digests rather than exposing the internal API. */
static void sha_known_answer(void) {
    char path[512];
    CnetHeldOut h;
    /* "abc" wrapped in a legal fixture would change the bytes, so hash the
       fixture itself and compare against a digest computed independently by
       tests/test_capability_cert_runner.py (hashlib) at gate time. Here we only
       assert the digest is a stable 64-hex string and changes with content. */
    char first[65], second[65];
    write_fixture("kat_a.json",
                  "{\"capability_id\":\"kat\",\"cases\":[{\"id\":\"c1\","
                  "\"n\":1}]}",
                  path, sizeof path);
    set_fixture(path);
    check(cnet_heldout_open(&h, "kat") == 0, "known-answer fixture opens");
    check(strlen(h.sha256) == 64, "digest is 64 hex characters");
    snprintf(first, sizeof first, "%s", h.sha256);
    cnet_heldout_close(&h);

    write_fixture("kat_b.json",
                  "{\"capability_id\":\"kat\",\"cases\":[{\"id\":\"c1\","
                  "\"n\":2}]}",
                  path, sizeof path);
    set_fixture(path);
    check(cnet_heldout_open(&h, "kat") == 0, "second fixture opens");
    snprintf(second, sizeof second, "%s", h.sha256);
    check(strcmp(first, second) != 0,
          "one changed byte changes the bound digest");
    cnet_heldout_close(&h);
}

static void standalone_mode(void) {
    CnetHeldOut h;
    char buf[32];
    set_fixture(NULL);
    check(cnet_heldout_open(&h, "anything") == 1,
          "no fixture declared reports standalone mode");
    check(cnet_heldout_num(&h, "nope", "nope", 7.5) == 7.5,
          "standalone numeric read returns the caller fallback");
    check(strcmp(cnet_heldout_str(&h, "nope", "nope", buf, sizeof buf,
                                  "fb"), "fb") == 0,
          "standalone string read returns the caller fallback");
    check(cnet_heldout_array_len(&h, "nope", "nope", 3) == 3,
          "standalone array read returns the caller fallback");
    check(cnet_heldout_finish(&h) == 0, "standalone finish succeeds");
    check(h.errors == 0, "standalone mode records no errors");
    cnet_heldout_close(&h);
}

static const char *k_good =
    "{\n"
    "  \"capability_id\": \"demo\",\n"
    "  \"cases\": [\n"
    "    {\"id\": \"alpha\", \"floor\": 0.45, \"expected\": \"abstain\",\n"
    "     \"refs\": [\"a\", \"b\", \"c\"], \"flag\": true},\n"
    "    {\"id\": \"beta\", \"floor\": 2, \"expected\": \"answer\"}\n"
    "  ],\n"
    "  \"expected_markers\": [\"M\"]\n"
    "}\n";

static void happy_path(void) {
    char path[512], buf[32];
    CnetHeldOut h;
    write_fixture("good.json", k_good, path, sizeof path);
    set_fixture(path);
    check(cnet_heldout_open(&h, "demo") == 0, "well-formed fixture opens");
    check(h.case_count == 2, "both cases are seen");
    check(strcmp(h.case_id[0], "alpha") == 0, "first case id parses");
    check(strcmp(h.case_id[1], "beta") == 0, "second case id parses");
    check(cnet_heldout_num(&h, "alpha", "floor", 0.0) == 0.45,
          "numeric field is read from the fixture, not the fallback");
    check(cnet_heldout_num(&h, "alpha", "flag", 0.0) == 1.0,
          "boolean true reads as 1");
    check(strcmp(cnet_heldout_str(&h, "alpha", "expected", buf, sizeof buf,
                                  "x"), "abstain") == 0,
          "string field is read from the fixture");
    check(cnet_heldout_array_len(&h, "alpha", "refs", 0) == 3,
          "array length is read from the fixture");
    check(cnet_heldout_num(&h, "beta", "floor", 0.0) == 2.0,
          "integer field reads exactly");
    cnet_heldout_verdict(&h, "alpha", 1);
    cnet_heldout_verdict(&h, "beta", 1);
    check(cnet_heldout_finish(&h) == 0, "fully consumed fixture finishes clean");
    check(h.errors == 0, "no errors on the happy path");
    cnet_heldout_close(&h);
}

static void negatives(void) {
    char path[512], buf[32];
    CnetHeldOut h;

    write_fixture("wrong_cap.json",
                  "{\"capability_id\":\"other\",\"cases\":[{\"id\":\"a\"}]}",
                  path, sizeof path);
    set_fixture(path);
    check(cnet_heldout_open(&h, "demo") == -1,
          "a fixture declaring another capability is refused");
    cnet_heldout_close(&h);

    set_fixture("/definitely/not/a/fixture.json");
    check(cnet_heldout_open(&h, "demo") == -1,
          "a missing fixture is refused, never treated as standalone");
    cnet_heldout_close(&h);

    write_fixture("no_id.json",
                  "{\"capability_id\":\"demo\",\"cases\":[{\"floor\":1}]}",
                  path, sizeof path);
    set_fixture(path);
    check(cnet_heldout_open(&h, "demo") == -1,
          "a case without a stable id is refused");
    cnet_heldout_close(&h);

    write_fixture("dup_id.json",
                  "{\"capability_id\":\"demo\",\"cases\":["
                  "{\"id\":\"a\"},{\"id\":\"a\"}]}",
                  path, sizeof path);
    set_fixture(path);
    check(cnet_heldout_open(&h, "demo") == -1, "duplicate case ids are refused");
    cnet_heldout_close(&h);

    write_fixture("no_cases.json",
                  "{\"capability_id\":\"demo\",\"cases\":[]}", path,
                  sizeof path);
    set_fixture(path);
    check(cnet_heldout_open(&h, "demo") == -1, "an empty case list is refused");
    cnet_heldout_close(&h);

    write_fixture("truncated.json", "{\"capability_id\":\"demo\",\"cases\":[{",
                  path, sizeof path);
    set_fixture(path);
    check(cnet_heldout_open(&h, "demo") == -1, "a truncated fixture is refused");
    cnet_heldout_close(&h);

    /* From here on the fixture is valid; the reader must still refuse to
       report success when the EVALUATOR misuses it. */
    write_fixture("good2.json", k_good, path, sizeof path);

    set_fixture(path);
    check(cnet_heldout_open(&h, "demo") == 0, "valid fixture reopens");
    (void)cnet_heldout_num(&h, "alpha", "floor", 0.0);
    cnet_heldout_verdict(&h, "alpha", 1);
    check(cnet_heldout_finish(&h) == -1,
          "a declared case nobody read fails the receipt");
    cnet_heldout_close(&h);

    set_fixture(path);
    check(cnet_heldout_open(&h, "demo") == 0, "valid fixture reopens again");
    (void)cnet_heldout_num(&h, "alpha", "floor", 0.0);
    (void)cnet_heldout_num(&h, "beta", "floor", 0.0);
    cnet_heldout_verdict(&h, "alpha", 1);
    cnet_heldout_verdict(&h, "beta", 0);
    check(cnet_heldout_finish(&h) == -1, "a failed case verdict fails the receipt");
    cnet_heldout_close(&h);

    set_fixture(path);
    check(cnet_heldout_open(&h, "demo") == 0, "valid fixture reopens once more");
    check(cnet_heldout_num(&h, "alpha", "absent_key", 9.0) == 9.0,
          "an absent key returns the fallback");
    check(h.errors == 1, "an absent key is recorded as an error");
    (void)cnet_heldout_num(&h, "beta", "floor", 0.0);
    cnet_heldout_verdict(&h, "alpha", 1);
    cnet_heldout_verdict(&h, "beta", 1);
    check(cnet_heldout_finish(&h) == -1, "an unresolved read fails the receipt");
    cnet_heldout_close(&h);

    set_fixture(path);
    check(cnet_heldout_open(&h, "demo") == 0, "valid fixture reopens for types");
    (void)cnet_heldout_str(&h, "alpha", "floor", buf, sizeof buf, "fb");
    check(h.errors == 1, "reading a number as a string is an error");
    check(cnet_heldout_num(&h, "alpha", "expected", 0.0) == 0.0,
          "reading a string as a number returns the fallback");
    check(h.errors == 2, "reading a string as a number is an error");
    cnet_heldout_verdict(&h, "gamma", 1);
    check(h.errors == 3, "a verdict for an unknown case is an error");
    cnet_heldout_close(&h);

    /* A case whose verdict was never recorded must not certify. */
    set_fixture(path);
    check(cnet_heldout_open(&h, "demo") == 0, "valid fixture reopens for verdicts");
    (void)cnet_heldout_num(&h, "alpha", "floor", 0.0);
    (void)cnet_heldout_num(&h, "beta", "floor", 0.0);
    cnet_heldout_verdict(&h, "alpha", 1);
    check(cnet_heldout_finish(&h) == -1,
          "a case with no verdict at all fails the receipt");
    cnet_heldout_close(&h);
}

int main(void) {
    scratch_dir = mkdtemp(dir_template);
    if (!scratch_dir) {
        fprintf(stderr, "FAIL: cannot create scratch directory\n");
        return 1;
    }
    sha_known_answer();
    standalone_mode();
    happy_path();
    negatives();
    set_fixture(NULL);
    if (failures) {
        printf("CNET_HELDOUT_TEST_FAIL checks=%d failures=%d\n", checks,
               failures);
        return 1;
    }
    printf("CNET_HELDOUT_TEST_PASS checks=%d\n", checks);
    return 0;
}
