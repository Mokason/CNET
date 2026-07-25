#include "../include/cnet_fault.h"
#include "../include/cnet_promote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg);     \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

int main(void) {
    char path[] = "/tmp/cnet_fault_test_XXXXXX";
    int fd = mkstemp(path);
    CnetFaultLog log;
    CnetFaultRecord rec, loaded[8];
    size_t n;
    CnetPromoteInput pin;
    CnetPromoteDecision dec;
    char delta_path[] = "/tmp/cnet_promote_delta_XXXXXX";
    int dfd;
    FILE *df;

    CHECK(fd >= 0, "mkstemp");
    close(fd);
    unlink(path);

    CHECK(cnet_fault_open(&log, path) == 0, "open");
    memset(&rec, 0, sizeof rec);
    rec.source = CNET_FAULT_SRC_JTC;
    snprintf(rec.unit, sizeof rec.unit, "json_toolcall_v2");
    snprintf(rec.skill, sizeof rec.skill, "jtc");
    snprintf(rec.session, sizeof rec.session, "s1");
    rec.in_dim = 18;
    rec.out_dim = 8;
    snprintf(rec.label_kind, sizeof rec.label_kind, "argmax");
    snprintf(rec.note, sizeof rec.note, "misclassified browser");
    CHECK(cnet_fault_append(&log, &rec) == 0, "append1");
    rec.source = CNET_FAULT_SRC_GHOST;
    snprintf(rec.unit, sizeof rec.unit, "acq_research_rpg_place_as_testimony");
    CHECK(cnet_fault_append(&log, &rec) == 0, "append2");
    cnet_fault_close(&log);

    CHECK(cnet_fault_count_file(path) == 2, "count");
    n = cnet_fault_load(path, "json_toolcall_v2", loaded, 8);
    CHECK(n == 1, "filter unit");
    CHECK(loaded[0].source == CNET_FAULT_SRC_JTC, "source");
    CHECK(loaded[0].in_dim == 18, "in_dim");
    n = cnet_fault_load(path, NULL, loaded, 8);
    CHECK(n == 2, "load all");

    cnet_promote_defaults(&pin);
    pin.fixes = 10;
    pin.regressions = 1;
    pin.min_net_gain = 5;
    dec = cnet_promote_decide(&pin);
    CHECK(dec.allowed == 1, "promote ok");
    pin.regressions = 8;
    pin.max_regressions = 2;
    dec = cnet_promote_decide(&pin);
    CHECK(dec.allowed == 0, "promote reject regressions");

    dfd = mkstemp(delta_path);
    CHECK(dfd >= 0, "delta mkstemp");
    df = fdopen(dfd, "w");
    fprintf(df, "delta 0.12\n");
    fclose(df);
    {
        double d = 0;
        CHECK(cnet_promote_read_eval_delta(delta_path, &d) == 0, "read delta");
        CHECK(d > 0.11 && d < 0.13, "delta val");
    }
    pin.fixes = 10;
    pin.regressions = 0;
    pin.max_regressions = -1;
    pin.require_eval_delta = 1;
    pin.eval_delta = 0.12;
    pin.min_eval_delta = 0.05;
    dec = cnet_promote_decide(&pin);
    CHECK(dec.allowed == 1, "promote with eval");
    pin.eval_delta = 0.01;
    dec = cnet_promote_decide(&pin);
    CHECK(dec.allowed == 0, "promote eval fail");

    /* vector round-trip */
    {
        CnetFaultLog log2;
        CnetFaultRecord r2;
        double in[4] = {1,0,1,0}, tg[2] = {0,1}, I[8], T[4];
        size_t nv;
        CHECK(cnet_fault_open(&log2, path) == 0, "reopen");
        memset(&r2, 0, sizeof r2);
        r2.source = CNET_FAULT_SRC_JTC;
        snprintf(r2.unit, sizeof r2.unit, "json_toolcall_v2");
        r2.in_dim = 4; r2.out_dim = 2;
        CHECK(cnet_fault_append_labeled(&log2, &r2, in, tg) == 0, "append_labeled");
        cnet_fault_close(&log2);
        nv = cnet_fault_load_vectors(path, "json_toolcall_v2", 4, 2, I, T, 2);
        CHECK(nv >= 1, "load_vectors");
        CHECK(I[0] == 1.0 && T[1] == 1.0, "vec values");
    }

    unlink(path);
    unlink(delta_path);

    if (g_fail) {
        fprintf(stderr, "cnet_fault_test failures=%d\n", g_fail);
        return 1;
    }
    /* dedupe: same labeled pair twice → one new line */
    {
        size_t before, after;
        CnetFaultRecord rr;
        double in2[4]={1,0,1,0}, tg2[2]={0,1};
        setenv("CNET_FAULT_LOG", path, 1);
        unsetenv("CNET_FAULT_DEDUPE");
        before = cnet_fault_count_file(path);
        memset(&rr,0,sizeof rr);
        rr.source=CNET_FAULT_SRC_JTC;
        snprintf(rr.unit,sizeof rr.unit,"dedup_unit");
        rr.in_dim=4; rr.out_dim=2;
        cnet_fault_mirror_labeled("dedup_unit", in2, tg2, 4, 2, "jtc");
        cnet_fault_mirror_labeled("dedup_unit", in2, tg2, 4, 2, "jtc");
        after = cnet_fault_count_file(path);
        CHECK(after == before + 1, "dedupe collapses second");
    }
    printf("CNET_FAULT_PASS checks=16\n");
    printf("CNET_PROMOTE_PASS checks=5\n");
    return 0;
}
