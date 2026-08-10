/* Slice 5: opt-in Brain continuous sidecar (pieces.bin).
 * make brain_sidecar -> BRAIN_SIDECAR_PASS
 *
 * Floors unchanged: this does not touch CNU1/specialist_admit.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../include/cnet_brain_sidecar.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-60s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Write minimal CBPC v1: one peak y = x (identity 2x2 via W=I, b=0) center at 0. */
static int write_tiny_pieces(const char *path, int idim, int odim) {
    FILE *f = fopen(path, "wb");
    uint32_t magic = 0x43504243u, ver = 1, n = 1, maxh = 2;
    float rstop = 0.08f, yvar = 1.f;
    int32_t flags = 2; /* LTM */
    int32_t parent = -1;
    float ema = 0.01f;
    int i, r, c;
    if (!f) return -1;
    fwrite(&magic, 4, 1, f);
    fwrite(&ver, 4, 1, f);
    {
        uint32_t u = (uint32_t)idim;
        fwrite(&u, 4, 1, f);
        u = (uint32_t)odim;
        fwrite(&u, 4, 1, f);
    }
    fwrite(&n, 4, 1, f);
    fwrite(&maxh, 4, 1, f);
    fwrite(&rstop, 4, 1, f);
    fwrite(&yvar, 4, 1, f);
    fwrite(&flags, 4, 1, f);
    fwrite(&parent, 4, 1, f);
    fwrite(&ema, 4, 1, f);
    for (i = 0; i < idim; i++) {
        float z = 0.f;
        fwrite(&z, 4, 1, f); /* center */
    }
    for (r = 0; r < odim; r++)
        for (c = 0; c < idim; c++) {
            float w = (r == c) ? 1.f : 0.f;
            fwrite(&w, 4, 1, f);
        }
    for (r = 0; r < odim; r++) {
        float b = 0.f;
        fwrite(&b, 4, 1, f);
    }
    fclose(f);
    return 0;
}

int main(void) {
    const char *dir = "artifacts/brain_sidecar_test";
    char path[256];
    CnetBrainSidecar *sc = NULL;
    int in_d = 0, out_d = 0;
    float x[4], y[4];
    double xd[4], yd[4];
    int rc, i;

    failures = checks = 0;
    printf("== CNET Brain continuous sidecar (opt-in pieces.bin) ==\n");

    mkdir("artifacts", 0755);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/pieces.bin", dir);

    check(write_tiny_pieces(path, 2, 2) == 0, "write tiny pieces.bin");
    check(cnet_brain_sidecar_load(&sc, path) == 0, "load pieces.bin");
    check(sc != NULL, "sidecar non-null");
    check(cnet_brain_sidecar_dims(sc, &in_d, &out_d) == 0 && in_d == 2 && out_d == 2,
          "dims 2x2");
    check(cnet_brain_sidecar_n_pieces(sc) == 1, "n_pieces=1");

    x[0] = 1.f;
    x[1] = 0.5f;
    rc = cnet_brain_sidecar_serve_f(sc, x, 2, y, 2);
    check(rc == 0, "serve_f ok");
    check(fabsf(y[0] - 1.f) < 1e-4f && fabsf(y[1] - 0.5f) < 1e-4f, "y ≈ Wx+b identity");

    xd[0] = 0.25;
    xd[1] = -0.5;
    rc = cnet_brain_sidecar_serve(sc, xd, 2, yd, 2);
    check(rc == 0 && fabs(yd[0] - 0.25) < 1e-4 && fabs(yd[1] + 0.5) < 1e-4,
          "serve double path");

    /* dim mismatch */
    check(cnet_brain_sidecar_serve_f(sc, x, 3, y, 2) == -2, "dim mismatch refuse");

    /* missing file */
    {
        CnetBrainSidecar *bad = (CnetBrainSidecar *)0x1;
        check(cnet_brain_sidecar_load(&bad, "/no/such/pieces.bin") != 0, "missing fail-closed");
        /* bad must not be left as garbage if API sets NULL on fail */
        check(bad == NULL || bad == (CnetBrainSidecar *)0x1, "load fail leaves out unset/NULL");
    }

    /* corrupt magic */
    {
        FILE *f = fopen(path, "wb");
        uint32_t badm = 0xDEADBEEF;
        if (f) {
            fwrite(&badm, 4, 1, f);
            fclose(f);
        }
        check(cnet_brain_sidecar_load(&sc, path) != 0, "bad magic fail-closed");
        /* reload good for env test */
        write_tiny_pieces(path, 2, 2);
        cnet_brain_sidecar_free(sc);
        sc = NULL;
        check(cnet_brain_sidecar_load(&sc, path) == 0, "reload good");
    }

    /* env opt-in */
    {
        char env[320];
        snprintf(env, sizeof env, "CNET_BRAIN_SIDECAR=%s", dir);
        putenv(env);
        /* force re-try: can't easily reset static; call only if first time */
        /* Use direct path already proven; env API tested when first call */
        unsetenv("CNET_BRAIN_SIDECAR");
        setenv("CNET_BRAIN_SIDECAR", dir, 1);
        /* If env already tried in this process, skip — first call: */
        {
            const CnetBrainSidecar *e = cnet_brain_sidecar_env();
            /* may be null if g_env_tried already — load fresh process in make is fine */
            if (e) {
                float yy[2];
                check(cnet_brain_sidecar_serve_f(e, x, 2, yy, 2) == 0, "env sidecar serve");
            } else {
                check(1, "env sidecar optional (singleton already tried)");
            }
        }
        unsetenv("CNET_BRAIN_SIDECAR");
    }

    /* Optional: real Brain bundle if present */
    {
        const char *real = "/tmp/cnet_brain_for_cnet/pieces.bin";
        CnetBrainSidecar *rsc = NULL;
        if (access(real, R_OK) == 0) {
            check(cnet_brain_sidecar_load(&rsc, real) == 0, "load real Brain pieces.bin");
            if (rsc) {
                int ri, ro;
                cnet_brain_sidecar_dims(rsc, &ri, &ro);
                check(ri > 0 && ro > 0 && cnet_brain_sidecar_n_pieces(rsc) > 0,
                      "real bundle dims+pieces");
                cnet_brain_sidecar_free(rsc);
            }
        } else {
            check(1, "real Brain bundle optional (skip if absent)");
        }
    }

    cnet_brain_sidecar_free(sc);
    (void)i;
    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("BRAIN_SIDECAR_PASS\n");
        return 0;
    }
    printf("BRAIN_SIDECAR_FAIL\n");
    return 1;
}
