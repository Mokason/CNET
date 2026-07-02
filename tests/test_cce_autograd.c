/* test_cce_autograd.c - native reverse mode autograd tests */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce.h"
#include "../include/cce/cce_autograd.h"

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

typedef struct {
    int calls;
    int num_inputs;
    int inputs_match;
    float out_grad0;
    cce_ag_tensor* expected_inputs[2];
} custom_probe;

static void custom_probe_backward(cce_ag_ctx* ctx,
                                  cce_ag_tensor* out_grad_owner,
                                  cce_ag_tensor** inputs,
                                  int num_inputs,
                                  void* user_ctx) {
    (void)ctx;
    custom_probe* probe = (custom_probe*)user_ctx;
    const float* out_grad = cce_ag_grad(out_grad_owner);
    if (!probe) return;

    probe->calls++;
    probe->num_inputs = num_inputs;
    probe->out_grad0 = out_grad ? out_grad[0] : -999.0f;
    probe->inputs_match =
        inputs &&
        num_inputs == 2 &&
        inputs[0] == probe->expected_inputs[0] &&
        inputs[1] == probe->expected_inputs[1] &&
        cce_ag_tensor_rows(inputs[0]) == 1 &&
        cce_ag_tensor_cols(inputs[0]) == 2 &&
        cce_ag_tensor_rows(inputs[1]) == 1 &&
        cce_ag_tensor_cols(inputs[1]) == 2;
}

static double finite_diff_matmul(int m, int k, int n, float eps) {
    cce_ag_ctx* ctx = NULL;
    if (cce_ag_create(&ctx, 256) != CCE_OK) return 1.0;

    float* A = (float*)calloc((size_t)m * k, sizeof(float));
    float* B = (float*)calloc((size_t)k * n, sizeof(float));
    float* target = (float*)calloc((size_t)m * n, sizeof(float));

    for (int i = 0; i < m*k; i++) A[i] = 0.05f * (float)((i % 5) - 2);
    for (int i = 0; i < k*n; i++) B[i] = 0.05f * (float)((i % 7) - 3);
    for (int i = 0; i < m*n; i++) target[i] = 0.3f;

    cce_ag_tensor *ta = NULL, *tb = NULL, *y = NULL, *ttgt = NULL, *tloss = NULL;

    cce_ag_tensor_from_array(ctx, A, m, k, 1, &ta);
    cce_ag_tensor_from_array(ctx, B, k, n, 0, &tb);
    cce_ag_tensor_from_array(ctx, target, m, n, 0, &ttgt);

    cce_ag_matmul(ctx, ta, tb, &y);
    cce_ag_mse_loss(ctx, y, ttgt, &tloss);

    cce_ag_backward(ctx, tloss);

    const float* g = cce_ag_grad(ta);
    double analytic = g ? g[0] : 0;

    float orig = A[0];
    A[0] = orig + eps;
    cce_ag_tensor* ta2 = NULL;
    cce_ag_tensor_from_array(ctx, A, m, k, 1, &ta2);
    cce_ag_tensor *y2 = NULL, *tl2 = NULL;
    cce_ag_matmul(ctx, ta2, tb, &y2);
    cce_ag_mse_loss(ctx, y2, ttgt, &tl2);
    double l1 = cce_ag_data(tl2)[0];

    A[0] = orig - eps;
    cce_ag_tensor* ta3 = NULL;
    cce_ag_tensor_from_array(ctx, A, m, k, 1, &ta3);
    cce_ag_tensor *y3 = NULL, *tl3 = NULL;
    cce_ag_matmul(ctx, ta3, tb, &y3);
    cce_ag_mse_loss(ctx, y3, ttgt, &tl3);
    double l2 = cce_ag_data(tl3)[0];

    double numeric = (l1 - l2) / (2.0 * eps);
    double rel_err = fabs(analytic - numeric) / (fabs(analytic) + 1e-6);

    cce_ag_destroy(ctx);
    free(A); free(B); free(target);
    return rel_err;
}

int main(void) {
    printf("=== cce_autograd hardened smoke + grad checks ===\n");

    {
        cce_ag_ctx* ctx = NULL;
        cce_result rc = cce_ag_create(&ctx, 128);
        CHECK(rc == CCE_OK, "create ctx");

        float data[6] = {1,2,3,4,5,6};
        cce_ag_tensor* t = NULL;
        rc = cce_ag_tensor_from_array(ctx, data, 2, 3, 1, &t);
        CHECK(rc == CCE_OK, "tensor from array");
        CHECK(cce_ag_data(t) != NULL, "data ptr");
        CHECK(cce_ag_tensor_rows(t) == 2 && cce_ag_tensor_cols(t) == 3, "shape");

        cce_ag_destroy(ctx);
    }

    {
        double err = finite_diff_matmul(2, 3, 2, 1e-3f);
        printf("  matmul grad rel err ~ %.4f\n", err);
        CHECK(err < 0.05, "matmul grad numeric");
    }

    /* Custom op callback dispatch + input/user metadata */
    {
        cce_ag_ctx* ctx = NULL;
        cce_result rc = cce_ag_create(&ctx, 32);
        CHECK(rc == CCE_OK, "custom op create ctx");

        float a_data[2] = {1.0f, 2.0f};
        float b_data[2] = {3.0f, 4.0f};
        cce_ag_tensor* ta = NULL;
        cce_ag_tensor* tb = NULL;
        cce_ag_tensor* out = NULL;
        cce_ag_tensor_from_array(ctx, a_data, 1, 2, 1, &ta);
        cce_ag_tensor_from_array(ctx, b_data, 1, 2, 0, &tb);

        cce_ag_tensor* inputs[2] = {ta, tb};
        custom_probe probe;
        memset(&probe, 0, sizeof(probe));
        probe.expected_inputs[0] = ta;
        probe.expected_inputs[1] = tb;

        rc = cce_ag_custom_op(ctx, inputs, 2, 1, 1, custom_probe_backward, &probe, &out);
        CHECK(rc == CCE_OK, "custom op create");
        CHECK(cce_ag_tensor_rows(out) == 1 && cce_ag_tensor_cols(out) == 1, "custom op output shape");
        rc = cce_ag_backward(ctx, out);
        CHECK(rc == CCE_OK, "custom op backward returns ok");
        CHECK(probe.calls == 1, "custom op backward callback called once");
        CHECK(probe.num_inputs == 2, "custom op backward receives all inputs");
        CHECK(probe.inputs_match, "custom op backward receives original inputs");
        CHECK(fabs(probe.out_grad0 - 1.0f) < 1e-6f, "custom op output grad is seeded");

        cce_ag_destroy(ctx);
    }

    /* Sigmoid numeric grad */
    {
        cce_ag_ctx* ctx = NULL; cce_ag_create(&ctx, 32);
        float x[1] = {0.5f};
        cce_ag_tensor* tx = NULL;
        cce_ag_tensor_from_array(ctx, x, 1, 1, 1, &tx);
        cce_ag_tensor* ty = NULL;
        cce_ag_sigmoid(ctx, tx, &ty);
        // fake loss = y
        cce_ag_backward(ctx, ty);  // seed would be 1 on ty grad

        // manual numeric
        float orig = x[0];
        float eps = 1e-4f;
        x[0] = orig + eps;
        cce_ag_tensor* tx2 = NULL;
        cce_ag_tensor_from_array(ctx, x, 1, 1, 1, &tx2);
        cce_ag_tensor* ty2 = NULL;
        cce_ag_sigmoid(ctx, tx2, &ty2);
        double l1 = cce_ag_data(ty2)[0];

        x[0] = orig - eps;
        cce_ag_tensor* tx3 = NULL;
        cce_ag_tensor_from_array(ctx, x, 1, 1, 1, &tx3);
        cce_ag_tensor* ty3 = NULL;
        cce_ag_sigmoid(ctx, tx3, &ty3);
        double l2 = cce_ag_data(ty3)[0];

        double numeric = (l1 - l2) / (2*eps);
        double analytic = cce_ag_grad(tx)[0];
        double rel = fabs(analytic - numeric) / (fabs(analytic)+1e-8);
        printf("  sigmoid grad rel err ~ %.4f\n", rel);
        CHECK(rel < 0.01, "sigmoid grad numeric");

        cce_ag_destroy(ctx);
    }

    printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
