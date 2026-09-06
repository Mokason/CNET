#include "cnet_roe_asi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
static int failures;
#define CHECK(x) do { if (!(x)) { printf("ROE_PUBLICATION_RED line=%d\n", __LINE__); failures++; } } while (0)
static int teach(RoeAsi *r, const char *q, const char *a) {
    RoeReply reply;
    CHECK(!roe_add_teach(r, q, "ad_hoc", a));
    roe_turn(r, q, &reply);
    return roe_feedback_verify(r, q, a, 1);
}
int main(void) {
    char root[] = "/tmp/cnet-roe-publication-XXXXXX", path[512];
    if (!mkdtemp(root)) return 2;
    RoeAsi *r = calloc(1, sizeof *r), *copy = calloc(1, sizeof *copy);
    roe_init(r); roe_set_catalog_dir(r, root);
    CHECK(teach(r, "first request", "first answer") == 1);
    CHECK(teach(r, "second request", "quoted \"answer\" with \\ slash") == 1);
    CHECK(r->n_skills == 2 && strcmp(r->skills[0].id, r->skills[1].id));
    roe_init(copy); roe_set_catalog_dir(copy, root);
    CHECK(roe_load_catalog(copy) == 2);
    CHECK(!strcmp(copy->skills[1].answer, "quoted \"answer\" with \\ slash"));
    CHECK(roe_load_catalog(copy) == 2 && copy->n_skills == 2);
    CHECK(roe_add_skill(copy, copy->skills[0].id, "ad_hoc", "different", "tampered", 1, 1) != 0);
    /* A legal decoded answer can expand sixfold when JSON-escaped. */
    char long_answer[ROE_ANSWER_MAX];
    const char escaped[] = {'"', '\\', '\n', '\001'};
    for (size_t i = 0; i < sizeof long_answer - 1; i++) long_answer[i] = escaped[i % sizeof escaped];
    long_answer[sizeof long_answer - 1] = 0;
    CHECK(!roe_add_skill(r, "long_escaped_answer", "ad_hoc", "long request", long_answer, 1, 1));
    CHECK(roe_save_catalog(r) == 3);
    CHECK(roe_load_catalog(copy) == 3 && copy->n_skills == 3);
    CHECK(copy->n_skills == 3 && !strcmp(copy->skills[2].answer, long_answer));
    snprintf(path, sizeof path, "%s/not_a_directory", root);
    FILE *f = fopen(path, "w"); if (!f) return 2; fclose(f);
    roe_set_catalog_dir(r, path);
    size_t count = r->n_skills; uint64_t promoted = r->n_promote;
    CHECK(teach(r, "third request", "third answer") == 0);
    CHECK(r->n_skills == count && r->n_promote == promoted);
    unlink(path);
    /* Only remove files in this test's private directory. */
    for (size_t i = 0; i < r->n_skills; i++) {
        snprintf(path, sizeof path, "%s/skills/%s/SKILL.roe", root, r->skills[i].id); unlink(path);
        snprintf(path, sizeof path, "%s/skills/%s/manifest.roe", root, r->skills[i].id); unlink(path);
        snprintf(path, sizeof path, "%s/skills/%s", root, r->skills[i].id); rmdir(path);
    }
    snprintf(path, sizeof path, "%s/skills", root); rmdir(path);
    snprintf(path, sizeof path, "%s/catalog.jsonl", root); unlink(path); rmdir(root);
    free(r); free(copy);
    printf("ROE_PUBLICATION_%s failures=%d\n", failures ? "RED" : "PASS", failures);
    return failures != 0;
}
