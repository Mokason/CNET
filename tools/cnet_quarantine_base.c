/* Offline operator migration. Stop writers first; sidecars are not migrated.
 * Reuse CNB subsets; publish both halves atomically, never overwrite a target. */
#include "base.h"
#include "hybrid_ai.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { const char *name; int exclude; } Selection;
static int selected(const char *name, void *ctx) {
    Selection *s = ctx;
    return (!strcmp(name, s->name)) != s->exclude;
}
static int exact_subset(const CnetBase *src, const CnetBase *dst) {
    for (size_t i = 0; i < dst->unit_count; i++) {
        const CnbUnitRef *u = &dst->units[i];
        size_t j;
        for (j = 0; j < src->unit_count; j++)
            if (!strcmp(src->units[j].name, u->name)) break;
        if (j == src->unit_count) return -1;
        const CnbUnitRef *v = &src->units[j];
        const CnbBlob *a = &src->blobs[v->blob_index], *b = &dst->blobs[u->blob_index];
        if (u->behavior_digest != v->behavior_digest || strcmp(u->provenance, v->provenance) ||
            a->len != b->len || memcmp(a->bytes, b->bytes, a->len)) return -1;
    }
    return 0;
}
int main(int argc, char **argv) {
    CnetBase src, part, probe;
    cnb_init(&src); cnb_init(&part); cnb_init(&probe);
    char stage[1200] = "", path[1300];
    int rc = 1, staged = 0;
    size_t kept = 0;
    if (argc == 3 && !strcmp(argv[1], "--initialize-empty-coverage")) {
        if (cnb_load(&src, argv[2])) goto done;
        for (size_t i = 0; i < src.unit_count; i++)
            if (hybrid_unit_is_mined(src.units[i].name)) goto done;
        if (snprintf(stage, sizeof stage, "%s.coverage.pending-XXXXXX", argv[2]) >= (int)sizeof stage) goto done;
        int fd = mkstemp(stage);
        if (fd < 0) goto done;
        close(fd);
        HybridAi empty; hybrid_ai_init(&empty);
        snprintf(path, sizeof path, "%s.coverage", argv[2]);
        rc = hybrid_coverage_save(&empty, stage) || renameat2(AT_FDCWD, stage, AT_FDCWD, path, RENAME_NOREPLACE);
        hybrid_ai_free(&empty); unlink(stage);
        if (!rc) puts("EMPTY_COVERAGE_INIT_PASS mined_units=0 records=0");
        goto done;
    }
    if (argc != 4) { fprintf(stderr, "usage: %s SOURCE UNIT NEW_DIRECTORY\n", argv[0]); goto done; }
    if (cnb_load(&src, argv[1]) || !cnb_has_unit(&src, argv[2])) goto done;
    int n = snprintf(stage, sizeof stage, "%s.pending-XXXXXX", argv[3]);
    if (n < 0 || (size_t)n >= sizeof stage || !mkdtemp(stage)) goto done;
    staged = 1;
    for (int i = 0; i < 2; i++) {
        Selection selection = {argv[2], i == 0};
        if (cnb_export_subset(&src, &part, selected, &selection) || exact_subset(&src, &part)) goto done;
        if (i == 0) kept = part.unit_count;
        if (part.unit_count != (i == 0 ? src.unit_count - 1 : 1)) goto done;
        snprintf(path, sizeof path, "%s/%s.cnb", stage, i == 0 ? "active" : "quarantined");
        if (cnb_save(&part, path) || cnb_load(&probe, path) || exact_subset(&src, &probe) ||
            probe.unit_count != part.unit_count) goto done;
        int fd = open(path, O_RDONLY | O_NOFOLLOW);
        if (fd < 0) goto done;
        int synced = fsync(fd); close(fd);
        if (synced) goto done;
        if (i == 0) {
            size_t mined = 0;
            for (size_t j = 0; j < part.unit_count; j++) mined += hybrid_unit_is_mined(part.units[j].name) != 0;
            if (!mined) {
                HybridAi empty; hybrid_ai_init(&empty);
                snprintf(path, sizeof path, "%s/active.cnb.coverage", stage);
                int saved = hybrid_coverage_save(&empty, path);
                hybrid_ai_free(&empty);
                if (saved) goto done;
            }
        }
        cnb_free(&part); cnb_init(&part); cnb_free(&probe); cnb_init(&probe);
    }
    int fd = open(stage, O_RDONLY | O_DIRECTORY);
    if (fd < 0) goto done;
    int synced = fsync(fd); close(fd);
    if (synced || renameat2(AT_FDCWD, stage, AT_FDCWD, argv[3], RENAME_NOREPLACE)) goto done;
    staged = 0; rc = 0;
    printf("BASE_QUARANTINE_PASS kept=%zu quarantined=1 byte_verified=1 source_unchanged=1\n", kept);
done:
    if (staged) {
        snprintf(path, sizeof path, "%s/active.cnb", stage); unlink(path);
        snprintf(path, sizeof path, "%s/quarantined.cnb", stage); unlink(path);
        snprintf(path, sizeof path, "%s/active.cnb.coverage", stage); unlink(path);
        rmdir(stage);
    }
    if (rc) fprintf(stderr, "BASE_QUARANTINE_REFUSED\n");
    cnb_free(&src); cnb_free(&part); cnb_free(&probe);
    return rc;
}
