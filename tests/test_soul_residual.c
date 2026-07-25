/* Live residual on soul_request + structure mine seal into CNB (hermetic).
 * make soul_residual_serve → SOUL_RESIDUAL_SERVE_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/soul_host.h"
#include "../include/base.h"
#include "../include/contract/contract.h"
#include "../include/nn.h"

#define SYM 4

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = SYM;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

int main(void) {
    const char *base_path = "tmp_soul_residual.cnb";
    BinaryTransformNetwork btn;
    Contract contract;
    CnetBase base;
    SoulHost *host = NULL;
    Port in_port = port("sr_in");
    Port out_port = port("sr_known");
    double in[SYM], out[SYM], expected[SYM];
    const double *raw;
    int reused = 0;
    int i;
    SoulServeStats st;

    remove(base_path);
    remove("tmp_soul_residual.cnb.tmp");
    remove("tmp_soul_residual.inbox");
    setenv("CNET_GAP_INBOX", "tmp_soul_residual.inbox", 1);
    setenv("CNET_SOUL_RESIDUAL_HERMETIC", "1", 1);
    setenv("CNET_SOUL_RESIDUAL_PREFER_HERMETIC", "1", 1);
    unsetenv("CNET_RESIDUAL_GGUF");

    printf("== soul residual live serve + structure mine ==\n");

    memset(&btn, 0, sizeof btn);
    check(btn_init(&btn, SYM, SYM, 4, 8, 0.1, 7u) == 0, "btn init");
    /* Identity-ish: argmax stays put via strong diagonal bias. */
    for (i = 0; i < SYM; i++) {
        btn.output_bias[i] = (i == 0) ? 10.0 : -10.0;
    }
    memset(btn.hidden_output_weights, 0,
           btn.hidden_count * btn.output_count * sizeof(double));
    check(btn_set_ports(&btn, in_port, out_port) == 0, "ports");
    for (i = 0; i < SYM; i++) in[i] = (i == 0) ? 1.0 : 0.0;
    raw = btn_forward(&btn, in);
    check(raw && port_canonicalize(out_port, raw, expected) == 0, "canon");
    check(contract_init_borrowed(&contract, "acq_sr_known", &btn, in, expected,
                                 1) == 0,
          "contract");
    check(btn_certify(&btn, &contract, NULL) == 0, "certify");
    cnb_init(&base);
    check(cnb_add_unit(&base, &btn, &contract, &reused) == 0, "add unit");
    check(cnb_save(&base, base_path) == 0, "save base");
    contract_free(&contract);
    btn_free(&btn);
    cnb_free(&base);

    check(soul_open(base_path, NULL, &host) == 0 && host, "soul_open");
    check(soul_serve_stats(host, &st) == 0, "stats after open");
    check(st.residual_bound == 1,
          "eager hermetic residual_bound on open (prefer+hermetic)");

    /* Known certified goal */
    {
        int rc = soul_request(host, PORT_ONEHOT, SYM, 1, "sr_in", PORT_ONEHOT,
                              SYM, 1, "sr_known", in, SYM, out, SYM);
        check(rc == SYM && soul_last_source(host) == SOUL_SOURCE_CERTIFIED,
              "certified serve");
    }

    /* Novel goal → residual (hermetic rot1) + gap note */
    for (i = 0; i < SYM; i++) in[i] = (i == 0) ? 1.0 : 0.0;
    {
        int rc = soul_request(host, PORT_ONEHOT, SYM, 1, "sr_in", PORT_ONEHOT,
                              SYM, 1, "sr_novel", in, SYM, out, SYM);
        check(rc == SYM && soul_last_source(host) == SOUL_SOURCE_RESIDUAL,
              "residual serves novel goal");
        /* hermetic residual: one-hot rotates by 1 → hot at 1 */
        check(out[1] == 1.0, "residual rot1 output");
    }
    check(access("tmp_soul_residual.inbox", 0) == 0 ||
              fopen("tmp_soul_residual.inbox", "r") != NULL,
          "gap inbox exists");
    {
        FILE *f = fopen("tmp_soul_residual.inbox", "r");
        char buf[256];
        int noted = 0;
        if (f) {
            while (fgets(buf, sizeof buf, f))
                if (strstr(buf, "NO_PLAN") || strstr(buf, "sr_novel"))
                    noted = 1;
            fclose(f);
        }
        check(noted, "gap inbox notes novel signature");
    }

    /* Hit residual enough times then structure-mine + seal */
    for (i = 0; i < 3; i++) {
        size_t j;
        for (j = 0; j < SYM; j++) in[j] = 0.0;
        in[i % SYM] = 1.0;
        soul_request(host, PORT_ONEHOT, SYM, 1, "sr_in", PORT_ONEHOT, SYM, 1,
                     "sr_novel", in, SYM, out, SYM);
    }
    {
        int units_before = soul_unit_count(host);
        int mrc = soul_structure_mine(host);
        check(mrc == 0, "structure mine seals unit");
        check(soul_unit_count(host) >= units_before, "registry grew or held");
    }
    /* After mine, novel signature may be local certified */
    for (i = 0; i < SYM; i++) in[i] = (i == 0) ? 1.0 : 0.0;
    {
        int rc = soul_request(host, PORT_ONEHOT, SYM, 1, "sr_in", PORT_ONEHOT,
                              SYM, 1, "sr_novel", in, SYM, out, SYM);
        check(rc == SYM, "post-mine serve succeeds");
        check(soul_last_source(host) == SOUL_SOURCE_CERTIFIED ||
                  soul_last_source(host) == SOUL_SOURCE_RESIDUAL,
              "post-mine certified or residual");
        if (soul_last_source(host) == SOUL_SOURCE_CERTIFIED)
            printf("  (post-mine source=certified Tier A)\n");
    }

    check(soul_serve_stats(host, &st) == 0, "stats ok");
    check(st.residual_serves >= 1 && st.gap_notes >= 1, "stats counters");
    printf("  stats: cert=%llu residual=%llu gaps=%llu mines=%llu seals=%llu "
           "units=%d\n",
           (unsigned long long)st.certified_serves,
           (unsigned long long)st.residual_serves,
           (unsigned long long)st.gap_notes,
           (unsigned long long)st.structure_mines,
           (unsigned long long)st.structure_seals, st.units);

    soul_close(host);
    remove(base_path);
    remove("tmp_soul_residual.cnb.tmp");
    remove("tmp_soul_residual.inbox");
    unsetenv("CNET_SOUL_RESIDUAL_HERMETIC");
    unsetenv("CNET_SOUL_RESIDUAL_PREFER_HERMETIC");
    unsetenv("CNET_GAP_INBOX");

    if (failures) {
        printf("SOUL_RESIDUAL_SERVE_FAIL failures=%d checks=%d\n", failures,
               checks);
        return 1;
    }
    printf("SOUL_RESIDUAL_SERVE_PASS checks=%d\n", checks);
    return 0;
}
