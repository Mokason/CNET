/* Reacquire a missing gate from the external teacher, never from local answers.
 * ALL sealed rows must match fresh external labels. No base weights are edited.
 * Publishes a NEW sidecar only; refuses replacement and partial agreement. */
#include "base.h"
#include "hybrid_ai.h"
#include "residual_http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int same(Port a, Port b) {
    return a.family == b.family && a.field_width == b.field_width &&
        a.field_count == b.field_count && !strcmp(a.tag, b.tag);
}
int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s BASE UNIT TEACHER_URL WINDOW_IDS NEW_SIDECAR\n", argv[0]); return 2;
    }
    CnetBase b; HybridAi h; BinaryTransformNetwork btn = {0}; Contract ct = {0};
    ResidualHttp *teacher = NULL;
    double *external = NULL;
    int rc = 1;
    size_t checked = 0;
    char tmp[1200] = "";
    cnb_init(&b); hybrid_ai_init(&h);
    if (cnb_load(&b, argv[1]) || cnb_get_unit(&b, argv[2], &btn, &ct) ||
        btn_certify(&btn, &ct, NULL) || btn.input_port_count != 1 || btn.output_port_count != 1 ||
        residual_http_open(&teacher, argv[3], argv[4], 0) ||
        !same(btn.input_ports[0], residual_http_input_port(teacher)) ||
        !same(btn.output_ports[0], residual_http_output_port(teacher))) goto done;
    external = calloc(btn.output_count, sizeof *external);
    if (!external || !ct.exemplar_count) goto done;
    for (size_t i = 0; i < ct.exemplar_count; i++) {
        if (residual_http_oracle(ct.inputs + i * btn.input_count, external, teacher) ||
            memcmp(external, ct.outputs + i * btn.output_count, btn.output_count * sizeof *external)) {
            fprintf(stderr, "COVERAGE_REVALIDATE_REFUSED row=%zu external_label_mismatch_or_unavailable\n", i); goto done;
        }
        checked++;
    }
    if (hybrid_coverage_record(&h, btn.input_ports[0], btn.output_ports[0], argv[2],
        ct.inputs, ct.outputs, ct.exemplar_count, btn.input_count, btn.output_count)) goto done;
    int n = snprintf(tmp, sizeof tmp, "%s.revalidate-XXXXXX", argv[5]);
    if (n < 0 || (size_t)n >= sizeof tmp) { tmp[0] = 0; goto done; }
    int fd = mkstemp(tmp);
    if (fd < 0) { tmp[0] = 0; goto done; }
    close(fd);
    if (hybrid_coverage_save(&h, tmp) ||
        renameat2(AT_FDCWD, tmp, AT_FDCWD, argv[5], RENAME_NOREPLACE)) goto done;
    tmp[0] = 0; rc = 0;
done:
    printf("COVERAGE_REVALIDATE_%s unit=%s externally_matched=%zu required=%zu base_unchanged=1\n",
        rc ? "REFUSED" : "PASS", argv[2], checked, ct.exemplar_count);
    if (tmp[0]) unlink(tmp);
    free(external); residual_http_close(teacher); contract_free(&ct); btn_free(&btn);
    hybrid_ai_free(&h); cnb_free(&b);
    return rc;
}
