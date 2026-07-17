#include "../include/external_residual.h"
#include "../include/personal_ai.h"

#include <stdio.h>
#include <string.h>

void external_residual_init(ExternalResidual *e) {
    if (e) memset(e, 0, sizeof *e);
}

int external_residual_bind(ExternalResidual *e, const char *name, Port in,
                           Port out, CnetOracleFn fn, void *ctx) {
    if (!e || !fn) return -1;
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", name ? name : "external_residual");
    e->in_port = in;
    e->out_port = out;
    e->fn = fn;
    e->ctx = ctx;
    e->bound = 1;
    return 0;
}

void external_residual_unbind(ExternalResidual *e) {
    if (e) memset(e, 0, sizeof *e);
}

int personal_ai_bind_external_residual(void *personal_ai, ExternalResidual *e) {
    PersonalAi *ai = (PersonalAi *)personal_ai;
    if (!ai || !e || !e->bound || !e->fn) return -1;
    return personal_ai_bind_residual(ai, e->name, e->fn, e->ctx);
}
