#ifndef CCE_MAPTRAIN_CPU_ONLY
#include "cce/cce_amdmath.h"
#endif
#include "cce/cce_maptrain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fill_f32(float *a, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++)
        a[i] = (float)((seed * 1103515245u + (unsigned)i * 12345u) % 1000) / 500.f - 1.f;
}

static void fill_f64(double *a, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++)
        a[i] = (double)((seed * 1103515245u + (unsigned)i * 12345u) % 1000) / 500.0 - 1.0;
}

int main(void)
{
    printf("cce_maptrain %s\n", cce_maptrain_version());
    const size_t E = 2, N = 4, In = 3, Out = 2;
    float Wf[2 * 2 * 3], Xf[2 * 4 * 3], dYf[2 * 4 * 2], Wf0[2 * 2 * 3];
    double Wd[2 * 2 * 3], Xd[2 * 4 * 3], dYd[2 * 4 * 2], Wd0[2 * 2 * 3];
    fill_f32(Wf, E * Out * In, 1);
    fill_f32(Xf, E * N * In, 2);
    fill_f32(dYf, E * N * Out, 3);
    memcpy(Wf0, Wf, sizeof Wf);
    fill_f64(Wd, E * Out * In, 1);
    fill_f64(Xd, E * N * In, 2);
    fill_f64(dYd, E * N * Out, 3);
    memcpy(Wd0, Wd, sizeof Wd);

    if (cce_maptrain_sgd(NULL, CCE_MAPTRAIN_F32, Wf, Xf, dYf, E, N, In, Out, 0.01) != 0) {
        printf("FAIL f32 cpu\n");
        return 1;
    }
    if (cce_maptrain_sgd(NULL, CCE_MAPTRAIN_F64, Wd, Xd, dYd, E, N, In, Out, 0.01) != 0) {
        printf("FAIL f64 cpu\n");
        return 1;
    }
    float g = 0.f;
    for (size_t n = 0; n < N; n++)
        g += Xf[n * In + 0] * dYf[n * Out + 0];
    float expect = Wf0[0] - 0.01f * g;
    float df = fabsf(expect - Wf[0]);
    double gd = 0.0;
    for (size_t n = 0; n < N; n++)
        gd += Xd[n * In + 0] * dYd[n * Out + 0];
    double expectd = Wd0[0] - 0.01 * gd;
    double dd = fabs(expectd - Wd[0]);
    printf("cpu f32 W[0] Δ=%g  f64 W[0] Δ=%g\n", df, dd);
    if (df > 1e-5f || dd > 1e-12) {
        printf("FAIL cpu ref\n");
        return 1;
    }
    int moved = 0;
    for (size_t i = 0; i < E * Out * In; i++) {
        if (Wf[i] != Wf0[i])
            moved = 1;
    }
    if (!moved) {
        printf("FAIL no update\n");
        return 1;
    }

#ifndef CCE_MAPTRAIN_CPU_ONLY
    cce_amdmath *h = cce_amdmath_open_device(0, NULL, 0);
    if (h) {
        const size_t Ef = 4, Nf = 256, Inf = 128, Outf = 128;
        size_t nw = Ef * Outf * Inf, nx = Ef * Nf * Inf, ny = Ef * Nf * Outf;
        float *Wf2 = (float *)malloc(nw * sizeof(float));
        float *Xf2 = (float *)malloc(nx * sizeof(float));
        float *Yf2 = (float *)malloc(ny * sizeof(float));
        float *Wref = (float *)malloc(nw * sizeof(float));
        fill_f32(Wf2, nw, 4);
        fill_f32(Xf2, nx, 5);
        fill_f32(Yf2, ny, 6);
        memcpy(Wref, Wf2, nw * sizeof(float));
        cce_maptrain_sgd(NULL, CCE_MAPTRAIN_F32, Wref, Xf2, Yf2, Ef, Nf, Inf, Outf, 0.001);
        int rc = cce_maptrain_sgd(h, CCE_MAPTRAIN_F32, Wf2, Xf2, Yf2, Ef, Nf, Inf, Outf, 0.001);
        float mx = 0.f;
        for (size_t i = 0; i < nw; i++) {
            float d = fabsf(Wf2[i] - Wref[i]);
            if (d > mx)
                mx = d;
        }
        printf("gpu f32 vs cpu max|Δ|=%g rc=%d\n", mx, rc);
        free(Wf2);
        free(Xf2);
        free(Yf2);
        free(Wref);
        if (mx > 1e-3f) {
            printf("FAIL gpu f32\n");
            cce_amdmath_close(h);
            return 1;
        }
        cce_amdmath_close(h);
    } else {
        printf("gpu skipped (no device)\n");
    }
#endif
    printf("MAPTRAIN_PASS\n");
    return 0;
}
