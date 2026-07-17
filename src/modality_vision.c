#include "../include/modality_vision.h"

#include <stdio.h>
#include <string.h>

static const char *const VISION_CLS[CNET_VISION_N_CLASS] = {
    "circle", "square", "triangle", "line"
};

const char *const *cnet_vision_class_names(void) { return VISION_CLS; }

void cnet_vision_hermetic_render(int class_id, double *pixels64) {
    int y, x;
    if (!pixels64) return;
    if (class_id < 0 || class_id >= CNET_VISION_N_CLASS) class_id = 0;
    memset(pixels64, 0, CNET_VISION_PIXELS * sizeof(double));
    switch (class_id) {
    case 0: /* circle-ish diamond */
        for (y = 0; y < CNET_VISION_SIDE; y++)
            for (x = 0; x < CNET_VISION_SIDE; x++) {
                int dy = y - 3, dx = x - 3;
                if (dy * dy + dx * dx <= 8)
                    pixels64[y * CNET_VISION_SIDE + x] = 1.0;
            }
        break;
    case 1: /* square */
        for (y = 1; y < CNET_VISION_SIDE - 1; y++)
            for (x = 1; x < CNET_VISION_SIDE - 1; x++)
                pixels64[y * CNET_VISION_SIDE + x] = 1.0;
        break;
    case 2: /* triangle */
        for (y = 1; y < CNET_VISION_SIDE - 1; y++)
            for (x = 0; x < CNET_VISION_SIDE; x++)
                if (x >= 3 - y / 2 && x <= 3 + y / 2)
                    pixels64[y * CNET_VISION_SIDE + x] = 1.0;
        break;
    default: /* horizontal line */
        for (x = 0; x < CNET_VISION_SIDE; x++)
            pixels64[4 * CNET_VISION_SIDE + x] = 1.0;
        break;
    }
}

int cnet_vision_hermetic_teacher(const double *in, double *out, void *ctx) {
    int c, best = 0;
    double best_d = 1e300;
    double img[CNET_VISION_PIXELS];
    (void)ctx;
    if (!in || !out) return -1;
    for (c = 0; c < CNET_VISION_N_CLASS; c++) {
        int k;
        double d = 0.0;
        cnet_vision_hermetic_render(c, img);
        for (k = 0; k < CNET_VISION_PIXELS; k++) {
            double e = in[k] - img[k];
            d += e * e;
        }
        if (d < best_d) {
            best_d = d;
            best = c;
        }
    }
    for (c = 0; c < CNET_VISION_N_CLASS; c++) out[c] = 0.0;
    out[best] = 1.0;
    return 0;
}

Port cnet_vision_input_port(void) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_RAW;
    p.field_width = CNET_VISION_PIXELS;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "image_px");
    return p;
}

Port cnet_vision_output_port(void) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = CNET_VISION_N_CLASS;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "vis_class");
    return p;
}

Port cnet_vision_evidence_port(void) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_EVIDENCE;
    p.field_width = CNET_VISION_N_CLASS;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "vis_class");
    return p;
}

int cnet_vision_v0_mine_admit(
    PrimitiveRegistry *reg,
    uint64_t identity_digest_salt,
    BinaryTransformNetwork **student_out,
    ExternalTeacher *teacher_out)
{
    ExternalTeacher local, *t;
    CnetOracleIdentity id;
    double inputs[CNET_VISION_N_CLASS * CNET_VISION_PIXELS];
    double targets[CNET_VISION_N_CLASS * CNET_VISION_N_CLASS];
    int c, j;

    if (!reg || !student_out) return -1;
    t = teacher_out ? teacher_out : &local;
    external_teacher_init(t);

    for (c = 0; c < CNET_VISION_N_CLASS; c++) {
        cnet_vision_hermetic_render(c, inputs + c * CNET_VISION_PIXELS);
        for (j = 0; j < CNET_VISION_N_CLASS; j++)
            targets[c * CNET_VISION_N_CLASS + j] = (j == c) ? 1.0 : 0.0;
    }

    memset(&id, 0, sizeof id);
    id.abi_version = CNET_ORACLE_ABI_VERSION;
    id.struct_size = (uint32_t)sizeof id;
    id.artifact_digest = identity_digest_salt
        ? identity_digest_salt
        : 0x564953494f4e5f30ULL; /* "VISION_0" */
    id.contract_digest = 0x5649535f434c5300ULL;
    id.config_digest = ((uint64_t)CNET_VISION_N_CLASS << 32) |
                       (uint64_t)CNET_VISION_PIXELS;

    if (external_teacher_bind_callback(
            t, CNET_MODALITY_VISION, "vision_hermetic_v0",
            cnet_vision_input_port(), cnet_vision_output_port(),
            cnet_vision_hermetic_teacher, NULL, &id, 0) != 0)
        return -2;

    if (external_teacher_mine_admit(
            t, reg, inputs, targets, CNET_VISION_N_CLASS, 16, 64, 15000, 5151u,
            "vision_class_v0", student_out) != 0) {
        if (!teacher_out) external_teacher_unbind(t);
        return 1;
    }
    if (!teacher_out) external_teacher_unbind(t);
    return 0;
}
