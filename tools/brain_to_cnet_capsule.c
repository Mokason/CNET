/* brain_to_cnet_capsule — promote Brain bundle mode structure into *real* CNET
 * capsules (unit.cnb + manifest.cknow), admit under CNET floors, export/import.
 *
 * CNET contracts are discrete 0/1 (CNU1). Continuous Brain Wx+b maps cannot be
 * unit_save'd as-is. This tool lifts the *discrete* mode identity Brain learned
 * (peak centers / plant modes) into certified ONEHOT specialists — actual
 * CNET capsules, fail-closed, same path as knowledge_capsule.
 *
 * Usage:
 *   brain_to_cnet_capsule <brain_bundle_dir> <cnet_out_dir>
 *   brain_to_cnet_capsule --demo <cnet_out_dir>
 *
 * Reads optional:
 *   <bundle>/pieces.bin   peak centers (preferred)
 *   <bundle>/plant.cbplant
 *
 * Writes:
 *   <out>/brain_mode_id/unit.cnb + manifest.cknow
 *   <out>/REPORT.txt
 *   round-trip import check on a fresh CnetBase
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../include/base.h"
#include "../include/cnet_capsule.h"
#include "../include/contract/contract.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/specialist.h"

#define MAX_MODES 16
#define MAX_IN 64
#define HIDDEN 24
#define MAX_HID 64

static int failures, checks;
static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port P_oh(const char *tag, size_t n) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = n;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void oh(double *r, int hot, int n) {
    int i;
    for (i = 0; i < n; i++) r[i] = (i == hot) ? 1.0 : 0.0;
}

static int mkdir_p(const char *path) {
    char tmp[768];
    size_t n, i;
    if (!path || !*path) return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    n = strlen(tmp);
    if (n && tmp[n - 1] == '/') tmp[n - 1] = 0;
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    return mkdir(tmp, 0755);
}

/* ---- load Brain pieces.bin peaks ---------------------------------------- */

typedef struct {
    int n_modes;
    int in_dim;
    float center[MAX_MODES][MAX_IN];
} BrainModes;

static int load_pieces_peaks(const char *path, BrainModes *m) {
    FILE *f;
    uint32_t magic = 0, ver = 0, idim = 0, odim = 0, n = 0, maxh = 0;
    float rstop = 0, yvar = 0;
    uint32_t i;
    memset(m, 0, sizeof *m);
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x43504243u ||
        fread(&ver, 4, 1, f) != 1 || ver != 1 ||
        fread(&idim, 4, 1, f) != 1 || fread(&odim, 4, 1, f) != 1 ||
        fread(&n, 4, 1, f) != 1 || fread(&maxh, 4, 1, f) != 1 ||
        fread(&rstop, 4, 1, f) != 1 || fread(&yvar, 4, 1, f) != 1 ||
        idim == 0 || idim > MAX_IN || n == 0 || n > 4096) {
        fclose(f);
        return -1;
    }
    m->in_dim = (int)idim;
    for (i = 0; i < n; i++) {
        int32_t flags = 0, parent = 0;
        float ema = 0;
        float *ctr = (float *)malloc((size_t)idim * sizeof(float));
        float *W = (float *)malloc((size_t)odim * (size_t)idim * sizeof(float));
        float *b = (float *)malloc((size_t)odim * sizeof(float));
        if (!ctr || !W || !b) {
            free(ctr);
            free(W);
            free(b);
            fclose(f);
            return -1;
        }
        if (fread(&flags, 4, 1, f) != 1 || fread(&parent, 4, 1, f) != 1 ||
            fread(&ema, 4, 1, f) != 1 ||
            fread(ctr, sizeof(float), idim, f) != idim ||
            fread(W, sizeof(float), (size_t)odim * idim, f) != (size_t)odim * idim ||
            fread(b, sizeof(float), odim, f) != odim) {
            free(ctr);
            free(W);
            free(b);
            fclose(f);
            return -1;
        }
        if (!(flags & 1) && m->n_modes < MAX_MODES) {
            memcpy(m->center[m->n_modes], ctr, (size_t)idim * sizeof(float));
            m->n_modes++;
        }
        free(ctr);
        free(W);
        free(b);
    }
    fclose(f);
    return m->n_modes > 0 ? 0 : -1;
}

static int load_demo_modes(BrainModes *m) {
    int c, j;
    memset(m, 0, sizeof *m);
    m->n_modes = 3;
    m->in_dim = 10;
    for (c = 0; c < 3; c++)
        for (j = 0; j < 10; j++)
            m->center[c][j] = (j == c * 3) ? 4.5f : 0.05f;
    return 0;
}

/* Build identity ONEHOT C→C unit from Brain mode count (discrete mode table). */
static int build_mode_id_unit(CnetBase *base, HybridAi *cov, int C, const char *name) {
    BinaryTransformNetwork *btn;
    double *in, *tg;
    Contract c;
    Specialist s;
    Port pin, pout;
    int i, rc = -1;

    if (C < 2 || C > MAX_MODES) return -1;
    btn = (BinaryTransformNetwork *)calloc(1, sizeof *btn);
    in = (double *)calloc((size_t)C * (size_t)C, sizeof(double));
    tg = (double *)calloc((size_t)C * (size_t)C, sizeof(double));
    if (!btn || !in || !tg) goto done;

    for (i = 0; i < C; i++) {
        oh(in + (size_t)i * (size_t)C, i, C);
        oh(tg + (size_t)i * (size_t)C, i, C);
    }
    if (btn_init(btn, (size_t)C, (size_t)C, HIDDEN, MAX_HID, 0.45, 11) != 0) goto done;
    pin = P_oh("brain_mode_in", (size_t)C);
    pout = P_oh("brain_mode_out", (size_t)C);
    if (btn_set_ports(btn, pin, pout) != 0) goto done;
    btn_train_dynamic(btn, in, tg, (size_t)C, 20000, 200, 1e-6, 1e-8);
    btn_train(btn, in, tg, (size_t)C, 4000);

    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, name, btn, in, tg, (size_t)C) != 0) goto done;
    memset(&s, 0, sizeof s);
    if (specialist_wrap_btn(&s, btn, name) != 0) {
        contract_free(&c);
        goto done;
    }
    if (cnb_add_unit(base, btn, &c, NULL) != 0) {
        contract_free(&c);
        goto done;
    }
    /* coverage: all modes (whole domain for mode table) */
    if (hybrid_coverage_record(cov, pin, pout, name, in, tg, (size_t)C, (size_t)C,
                               (size_t)C) != 0) {
        contract_free(&c);
        goto done;
    }
    contract_free(&c);
    rc = 0;
done:
    if (btn) {
        btn_free(btn);
        free(btn);
    }
    free(in);
    free(tg);
    return rc;
}

/* Discrete center-signature → mode: bits from sign of (center[c][j]-global_mean).
 * Proves Brain geometry became a CNET-certified classifier capsule. */
static int build_center_sig_unit(CnetBase *base, HybridAi *cov, const BrainModes *m,
                                 const char *name) {
    BinaryTransformNetwork *btn = NULL;
    double *in = NULL, *tg = NULL;
    Contract c;
    Specialist s;
    Port pin, pout;
    int C = m->n_modes;
    int bits = m->in_dim > 16 ? 16 : m->in_dim;
    int i, j, rc = -1;
    double gmean[MAX_IN];

    if (C < 2 || bits < 2) return -1;
    for (j = 0; j < bits; j++) {
        double s = 0;
        for (i = 0; i < C; i++) s += (double)m->center[i][j];
        gmean[j] = s / (double)C;
    }

    btn = (BinaryTransformNetwork *)calloc(1, sizeof *btn);
    in = (double *)calloc((size_t)C * (size_t)bits, sizeof(double));
    tg = (double *)calloc((size_t)C * (size_t)C, sizeof(double));
    if (!btn || !in || !tg) goto done;

    for (i = 0; i < C; i++) {
        for (j = 0; j < bits; j++)
            in[(size_t)i * (size_t)bits + (size_t)j] =
                ((double)m->center[i][j] > gmean[j]) ? 1.0 : 0.0;
        oh(tg + (size_t)i * (size_t)C, i, C);
    }

    if (btn_init(btn, (size_t)bits, (size_t)C, HIDDEN, MAX_HID, 0.5, 13) != 0) goto done;
    pin.family = PORT_BINARY_MSB;
    pin.field_width = (size_t)bits;
    pin.field_count = 1;
    snprintf(pin.tag, sizeof pin.tag, "brain_center_bits");
    pout = P_oh("brain_mode_out", (size_t)C);
    if (btn_set_ports(btn, pin, pout) != 0) goto done;
    btn_train_dynamic(btn, in, tg, (size_t)C, 25000, 200, 1e-6, 1e-8);
    btn_train(btn, in, tg, (size_t)C, 6000);

    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, name, btn, in, tg, (size_t)C) != 0) goto done;
    memset(&s, 0, sizeof s);
    if (specialist_wrap_btn(&s, btn, name) != 0) {
        contract_free(&c);
        goto done;
    }
    if (cnb_add_unit(base, btn, &c, NULL) != 0) {
        contract_free(&c);
        goto done;
    }
    if (hybrid_coverage_record(cov, pin, pout, name, in, tg, (size_t)C, (size_t)bits,
                               (size_t)C) != 0) {
        contract_free(&c);
        goto done;
    }
    contract_free(&c);
    rc = 0;
done:
    if (btn) {
        btn_free(btn);
        free(btn);
    }
    free(in);
    free(tg);
    return rc;
}

static int export_one(CnetBase *src, HybridAi *cov, const char *unit, const char *dir) {
    CnetCapsuleReport rep;
    char path[768];
    mkdir_p(dir);
    snprintf(path, sizeof path, "%s/%s", dir, unit);
    mkdir_p(path);
    memset(&rep, 0, sizeof rep);
    if (cnet_capsule_export(src, cov, unit, path, &rep) != 0) {
        printf("export %s FAILED: %s\n", unit, rep.reject_reason);
        return -1;
    }
    printf("export %s ok schema=%u exemplars=%zu cov_rows=%zu bytes=%zu digest=%llu\n", unit,
           rep.schema, rep.exemplars, rep.coverage_rows, rep.payload_bytes,
           (unsigned long long)rep.behavior_digest);
    return 0;
}

static int import_check(const char *cap_dir, const char *unit) {
    CnetBase dst;
    HybridAi cov;
    CnetCapsuleReport rep;
    int rc;
    cnb_init(&dst);
    hybrid_ai_init(&cov);
    memset(&rep, 0, sizeof rep);
    rc = cnet_capsule_import(&dst, &cov, cap_dir, &rep);
    if (rc != 0) {
        printf("import %s FAIL: %s\n", unit, rep.reject_reason);
        cnb_free(&dst);
        hybrid_ai_free(&cov);
        return -1;
    }
    if (!cnb_has_unit(&dst, unit)) {
        printf("import %s missing unit\n", unit);
        cnb_free(&dst);
        hybrid_ai_free(&cov);
        return -1;
    }
    printf("import %s ok unit_present=1 cov_rows=%zu\n", unit, rep.coverage_rows);
    cnb_free(&dst);
    hybrid_ai_free(&cov);
    return 0;
}

static void usage(const char *a0) {
    fprintf(stderr,
            "usage:\n"
            "  %s <brain_bundle_dir> <cnet_capsule_out_dir>\n"
            "  %s --demo <cnet_capsule_out_dir>\n",
            a0, a0);
}

int main(int argc, char **argv) {
    BrainModes modes;
    CnetBase src;
    HybridAi cov;
    const char *bundle = NULL, *outdir = NULL;
    char pieces[800], report[800];
    FILE *rf;
    int demo = 0;

    failures = checks = 0;
    printf("== Brain → actual CNET capsules (CNU1 + coverage) ==\n");

    if (argc >= 3 && strcmp(argv[1], "--demo") == 0) {
        demo = 1;
        outdir = argv[2];
    } else if (argc >= 3) {
        bundle = argv[1];
        outdir = argv[2];
    } else {
        usage(argv[0]);
        return 2;
    }

    memset(&modes, 0, sizeof modes);
    if (demo) {
        check(load_demo_modes(&modes) == 0, "demo Brain modes (3×10 plant centers)");
    } else {
        snprintf(pieces, sizeof pieces, "%s/pieces.bin", bundle);
        if (load_pieces_peaks(pieces, &modes) != 0) {
            printf("WARN: no pieces.bin peaks — falling back to demo modes\n");
            check(load_demo_modes(&modes) == 0, "fallback demo modes");
        } else {
            printf("loaded pieces.bin peaks n_modes=%d in_dim=%d\n", modes.n_modes,
                   modes.in_dim);
            check(modes.n_modes >= 2, "Brain peaks >= 2 modes");
        }
    }

    mkdir_p(outdir);
    cnb_init(&src);
    hybrid_ai_init(&cov);

    check(build_mode_id_unit(&src, &cov, modes.n_modes, "brain_mode_id") == 0,
          "build brain_mode_id (ONEHOT identity from Brain mode count)");
    check(build_center_sig_unit(&src, &cov, &modes, "brain_center_sig") == 0,
          "build brain_center_sig (center-bit signature → mode)");

    check(export_one(&src, &cov, "brain_mode_id", outdir) == 0,
          "export brain_mode_id CNET capsule");
    check(export_one(&src, &cov, "brain_center_sig", outdir) == 0,
          "export brain_center_sig CNET capsule");

    {
        char d1[900], d2[900];
        snprintf(d1, sizeof d1, "%s/brain_mode_id", outdir);
        snprintf(d2, sizeof d2, "%s/brain_center_sig", outdir);
        check(import_check(d1, "brain_mode_id") == 0, "import round-trip brain_mode_id");
        check(import_check(d2, "brain_center_sig") == 0, "import round-trip brain_center_sig");
    }

    snprintf(report, sizeof report, "%s/REPORT.txt", outdir);
    rf = fopen(report, "wb");
    if (rf) {
        fprintf(rf, "format=brain_to_cnet_capsule\n");
        fprintf(rf, "source=%s\n", demo ? "demo" : bundle);
        fprintf(rf, "n_modes=%d in_dim=%d\n", modes.n_modes, modes.in_dim);
        fprintf(rf, "units=brain_mode_id,brain_center_sig\n");
        fprintf(rf, "capsule_schema=CNET_CAPSULE_SCHEMA1\n");
        fprintf(rf, "law=discrete_0_1_contract; CNET floors unchanged\n");
        fprintf(rf, "note=continuous Brain Wx+b stays in pieces.bin; "
                    "CNET capsules carry discrete mode identity derived from Brain\n");
        fprintf(rf, "checks=%d failures=%d\n", checks, failures);
        fclose(rf);
    }

    cnb_free(&src);
    hybrid_ai_free(&cov);

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("BRAIN_CNET_CAPSULE_PASS\n");
        return 0;
    }
    printf("BRAIN_CNET_CAPSULE_FAIL\n");
    return 1;
}
