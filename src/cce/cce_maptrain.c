/* Stacked same-shape map SGD. float→optional GPU; double→CPU. Not CERT. */
#include "cce/cce_maptrain.h"

#ifdef CCE_MAPTRAIN_CPU_ONLY
static int cce_amdmath_sgd_f32(cce_amdmath *h, float *W, const float *X, const float *dY,
                               size_t E, size_t N, size_t in_dim, size_t out_dim, float lr)
{
    (void)h;
    (void)W;
    (void)X;
    (void)dY;
    (void)E;
    (void)N;
    (void)in_dim;
    (void)out_dim;
    (void)lr;
    return -1;
}
static int cce_amdmath_sgd_f64(cce_amdmath *h, double *W, const double *X, const double *dY,
                               size_t E, size_t N, size_t in_dim, size_t out_dim, double lr)
{
    (void)h;
    (void)W;
    (void)X;
    (void)dY;
    (void)E;
    (void)N;
    (void)in_dim;
    (void)out_dim;
    (void)lr;
    return -1;
}
static int cce_amdmath_below_floor(size_t a, size_t b, size_t c)
{
    (void)a;
    (void)b;
    (void)c;
    return 1;
}
#else
#include "cce/cce_amdmath.h"
#endif

const char *cce_maptrain_version(void)
{
    return CCE_MAPTRAIN_VERSION;
}

static void sgd_f32_cpu(float *W, const float *X, const float *dY, size_t E, size_t N,
                        size_t in_dim, size_t out_dim, float lr)
{
    for (size_t e = 0; e < E; e++) {
        float *We = W + e * out_dim * in_dim;
        const float *Xe = X + e * N * in_dim;
        const float *Ye = dY + e * N * out_dim;
        for (size_t o = 0; o < out_dim; o++) {
            for (size_t i = 0; i < in_dim; i++) {
                float g = 0.f;
                for (size_t n = 0; n < N; n++)
                    g += Xe[n * in_dim + i] * Ye[n * out_dim + o];
                We[o * in_dim + i] -= lr * g;
            }
        }
    }
}

static void sgd_f64_cpu(double *W, const double *X, const double *dY, size_t E, size_t N,
                        size_t in_dim, size_t out_dim, double lr)
{
    for (size_t e = 0; e < E; e++) {
        double *We = W + e * out_dim * in_dim;
        const double *Xe = X + e * N * in_dim;
        const double *Ye = dY + e * N * out_dim;
        for (size_t o = 0; o < out_dim; o++) {
            for (size_t i = 0; i < in_dim; i++) {
                double g = 0.0;
                for (size_t n = 0; n < N; n++)
                    g += Xe[n * in_dim + i] * Ye[n * out_dim + o];
                We[o * in_dim + i] -= lr * g;
            }
        }
    }
}

int cce_maptrain_sgd(cce_amdmath *gpu, int dtype, void *W, const void *X, const void *dY,
                     size_t E, size_t N, size_t in_dim, size_t out_dim, double lr)
{
    if (!W || !X || !dY || E == 0 || N == 0 || in_dim == 0 || out_dim == 0)
        return CCE_MAPTRAIN_ERR;
    if (dtype == CCE_MAPTRAIN_F64) {
        if (gpu && !cce_amdmath_below_floor(out_dim, in_dim, N)) {
            if (cce_amdmath_sgd_f64(gpu, (double *)W, (const double *)X, (const double *)dY, E,
                                    N, in_dim, out_dim, lr) == 0)
                return CCE_MAPTRAIN_OK;
        }
        sgd_f64_cpu((double *)W, (const double *)X, (const double *)dY, E, N, in_dim,
                    out_dim, lr);
        return CCE_MAPTRAIN_OK;
    }
    if (dtype != CCE_MAPTRAIN_F32)
        return CCE_MAPTRAIN_ERR;
    if (gpu && !cce_amdmath_below_floor(out_dim, in_dim, N)) {
        if (cce_amdmath_sgd_f32(gpu, (float *)W, (const float *)X, (const float *)dY, E, N,
                                in_dim, out_dim, (float)lr) == 0)
            return CCE_MAPTRAIN_OK;
    }
    sgd_f32_cpu((float *)W, (const float *)X, (const float *)dY, E, N, in_dim, out_dim,
                (float)lr);
    return CCE_MAPTRAIN_OK;
}
