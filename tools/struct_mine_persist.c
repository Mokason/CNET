/* Offline residual structure mining. Never reconstruct labels after mining. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include "personal_ai.h"
#include "residual_http.h"

static int persist_student(PersonalAi *ai, BinaryTransformNetwork *stu, const char *owner) {
    char coverage[600];
    CnbMark mark;
    cnb_mark(&ai->lane.base, &mark);
    if (hybrid_seal_mined_unit_owned(&ai->hybrid, &ai->lane.base, stu, owner, NULL)) return -1;
    snprintf(coverage, sizeof coverage, "%s.coverage", ai->lane.base_path);
    /* Guard before base: failure must never publish an unguarded unit.
     * Extra guard records after a failed checkpoint confer no authority. */
    if (hybrid_coverage_save(&ai->hybrid, coverage)) {
        (void)cnb_rollback(&ai->lane.base, &mark);
        return -1;
    }
    return gap_lane_checkpoint(&ai->lane);
}

int main(void) {
    PersonalAi ai; PersonalAiPolicy pol;
    char ledger[512], inbox[512], lock[600];
    const char *base = getenv("CNET_BASE_PATH");
    if (!base || !base[0]) { puts("STRUCT_MINE_PERSIST_REFUSED explicit_base_required"); return 2; }
    if (snprintf(ledger, sizeof ledger, "%s.gaps.txt", base) >= (int)sizeof ledger ||
        snprintf(inbox, sizeof inbox, "%s.inbox", base) >= (int)sizeof inbox) return 2;
    snprintf(lock, sizeof lock, "%s.writer.lock", base);
    int fd = open(lock, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) {
        if (fd >= 0) close(fd);
        puts("STRUCT_MINE_PERSIST_REFUSED writer_busy"); return 3;
    }
    setenv("CNET_STRUCTURE_EXPAND_N", "32", 0);
    personal_ai_policy_defaults(&pol); pol.structure_min_hits = 2;
    if (personal_ai_open(&ai, base, ledger, inbox, &pol)) { close(fd); return 2; }
    int rc = 1;
    ResidualHttp *rh = ai.owned_residual_http;
    double *in = NULL, *out = NULL;
    if (!rh || !ai.hybrid.residual.bound) goto done;
    size_t width = (size_t)residual_http_window_n(rh);
    if (!width) goto done;
    in = calloc(width, sizeof *in); out = calloc(width, sizeof *out);
    if (!in || !out) goto done;
    for (size_t i = 0; i < 6 && i < width; i++) {
        memset(in, 0, width * sizeof *in); in[i] = 1;
        if (hybrid_try_residual(&ai.hybrid, residual_http_input_port(rh),
            residual_http_output_port(rh), in, width, out, width)) goto done;
    }
    BinaryTransformNetwork *student = NULL;
    if (hybrid_structure_mine(&ai.hybrid, &ai.lane.reg, 2, &student) || !student) goto done;
    const char *owner = NULL;
    for (size_t i = 0; i < ai.lane.reg.count; i++)
        if (ai.lane.reg.entries[i].btn == student) owner = ai.lane.reg.entries[i].name;
    if (!owner || persist_student(&ai, student, owner)) goto done;
    CnetBase probe; cnb_init(&probe);
    rc = cnb_load(&probe, base) || !cnb_has_unit(&probe, owner);
    cnb_free(&probe);
done:
    free(in); free(out); personal_ai_close(&ai); close(fd);
    puts(rc ? "STRUCT_MINE_PERSIST_REFUSED" : "STRUCT_MINE_PERSIST_PASS");
    return rc;
}
