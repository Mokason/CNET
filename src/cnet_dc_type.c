#include "../include/cnet_dc_type.h"

#include <stdio.h>
#include <string.h>

void cnet_dc_arena_init(CnetDcArena *arena) {
    if (arena == NULL) return;
    memset(arena, 0, sizeof *arena);
}

void cnet_dc_ctx_init(CnetDcContext *ctx) {
    if (ctx == NULL) return;
    memset(ctx, 0, sizeof *ctx);
}

static int alloc_node(CnetDcArena *arena) {
    if (arena == NULL || arena->count >= CNET_DC_MAX_NODES) return -1;
    memset(&arena->nodes[arena->count], 0, sizeof arena->nodes[0]);
    return arena->count++;
}

static int valid(const CnetDcArena *arena, int type) {
    return arena != NULL && type >= 0 && type < arena->count;
}

static int is_polymorphic(const CnetDcArena *arena, int type) {
    const CnetDcNode *n;
    int i;
    if (!valid(arena, type)) return 0;
    n = &arena->nodes[type];
    if (n->kind == CNET_DC_VAR) return 1;
    for (i = 0; i < n->n_args; ++i) {
        if (is_polymorphic(arena, n->args[i])) return 1;
    }
    return 0;
}

int cnet_dc_var(CnetDcArena *arena, int index) {
    int id = alloc_node(arena);
    if (id < 0) return -1;
    arena->nodes[id].kind = CNET_DC_VAR;
    arena->nodes[id].var = index;
    return id;
}

int cnet_dc_ctor(CnetDcArena *arena, const char *name, const int *args,
                 int n_args) {
    int id, i;
    if (name == NULL || name[0] == '\0' || n_args < 0 ||
        n_args > CNET_DC_MAX_ARGS)
        return -1;
    if (n_args > 0 && args == NULL) return -1;
    for (i = 0; i < n_args; ++i) {
        if (!valid(arena, args[i])) return -1;
    }
    id = alloc_node(arena);
    if (id < 0) return -1;
    arena->nodes[id].kind = CNET_DC_CTOR;
    snprintf(arena->nodes[id].name, sizeof arena->nodes[id].name, "%s", name);
    arena->nodes[id].n_args = n_args;
    for (i = 0; i < n_args; ++i) arena->nodes[id].args[i] = args[i];
    return id;
}

int cnet_dc_base(CnetDcArena *arena, const char *name) {
    return cnet_dc_ctor(arena, name, NULL, 0);
}

int cnet_dc_arrow(CnetDcArena *arena, int domain, int range) {
    int args[2];
    args[0] = domain;
    args[1] = range;
    return cnet_dc_ctor(arena, CNET_DC_ARROW_NAME, args, 2);
}

int cnet_dc_list(CnetDcArena *arena, int elem) {
    return cnet_dc_ctor(arena, "list", &elem, 1);
}

int cnet_dc_pair(CnetDcArena *arena, int left, int right) {
    int args[2];
    args[0] = left;
    args[1] = right;
    return cnet_dc_ctor(arena, "pair", args, 2);
}

int cnet_dc_is_arrow(const CnetDcArena *arena, int type) {
    if (!valid(arena, type)) return 0;
    return arena->nodes[type].kind == CNET_DC_CTOR &&
           strcmp(arena->nodes[type].name, CNET_DC_ARROW_NAME) == 0 &&
           arena->nodes[type].n_args == 2;
}

int cnet_dc_equal(const CnetDcArena *arena, int a, int b) {
    const CnetDcNode *na, *nb;
    int i;
    if (!valid(arena, a) || !valid(arena, b)) return 0;
    if (a == b) return 1;
    na = &arena->nodes[a];
    nb = &arena->nodes[b];
    if (na->kind != nb->kind) return 0;
    if (na->kind == CNET_DC_VAR) return na->var == nb->var;
    if (strcmp(na->name, nb->name) != 0 || na->n_args != nb->n_args) return 0;
    for (i = 0; i < na->n_args; ++i) {
        if (!cnet_dc_equal(arena, na->args[i], nb->args[i])) return 0;
    }
    return 1;
}

static int show_rec(const CnetDcArena *arena, int type, int is_return,
                    char *out, size_t cap, size_t *used) {
    const CnetDcNode *n;
    int i, rc;
    size_t left;
    if (!valid(arena, type) || out == NULL || cap == 0 || used == NULL)
        return -1;
    left = cap - *used;
    if (left == 0) return -1;
    n = &arena->nodes[type];
    if (n->kind == CNET_DC_VAR) {
        rc = snprintf(out + *used, left, "t%d", n->var);
        if (rc < 0 || (size_t)rc >= left) return -1;
        *used += (size_t)rc;
        return 0;
    }
    if (cnet_dc_is_arrow(arena, type)) {
        if (!is_return) {
            if (*used + 1 >= cap) return -1;
            out[(*used)++] = '(';
            out[*used] = '\0';
        }
        if (show_rec(arena, n->args[0], 0, out, cap, used) != 0) return -1;
        left = cap - *used;
        rc = snprintf(out + *used, left, " -> ");
        if (rc < 0 || (size_t)rc >= left) return -1;
        *used += (size_t)rc;
        if (show_rec(arena, n->args[1], 1, out, cap, used) != 0) return -1;
        if (!is_return) {
            if (*used + 1 >= cap) return -1;
            out[(*used)++] = ')';
            out[*used] = '\0';
        }
        return 0;
    }
    if (n->n_args == 0) {
        rc = snprintf(out + *used, left, "%s", n->name);
        if (rc < 0 || (size_t)rc >= left) return -1;
        *used += (size_t)rc;
        return 0;
    }
    rc = snprintf(out + *used, left, "%s(", n->name);
    if (rc < 0 || (size_t)rc >= left) return -1;
    *used += (size_t)rc;
    for (i = 0; i < n->n_args; ++i) {
        if (i > 0) {
            left = cap - *used;
            rc = snprintf(out + *used, left, ", ");
            if (rc < 0 || (size_t)rc >= left) return -1;
            *used += (size_t)rc;
        }
        if (show_rec(arena, n->args[i], 1, out, cap, used) != 0) return -1;
    }
    if (*used + 1 >= cap) return -1;
    out[(*used)++] = ')';
    out[*used] = '\0';
    return 0;
}

int cnet_dc_show(const CnetDcArena *arena, int type, char *out, size_t cap) {
    size_t used = 0;
    if (out == NULL || cap == 0) return -1;
    out[0] = '\0';
    return show_rec(arena, type, 1, out, cap, &used);
}

static int lookup_subst(const CnetDcContext *ctx, int var) {
    int i;
    if (ctx == NULL) return -1;
    for (i = 0; i < ctx->subst_count; ++i) {
        if (ctx->subst_var[i] == var) return ctx->subst_type[i];
    }
    return -1;
}

int cnet_dc_apply(CnetDcArena *arena, const CnetDcContext *ctx, int type,
                  int *out) {
    const CnetDcNode *n;
    int args[CNET_DC_MAX_ARGS];
    int i, mapped, built;
    if (out == NULL || !valid(arena, type) || ctx == NULL) return -1;
    n = &arena->nodes[type];
    if (!is_polymorphic(arena, type)) {
        *out = type;
        return 0;
    }
    if (n->kind == CNET_DC_VAR) {
        mapped = lookup_subst(ctx, n->var);
        if (mapped < 0) {
            *out = type;
            return 0;
        }
        return cnet_dc_apply(arena, ctx, mapped, out);
    }
    if (n->n_args == 0) {
        *out = type;
        return 0;
    }
    for (i = 0; i < n->n_args; ++i) {
        if (cnet_dc_apply(arena, ctx, n->args[i], &args[i]) != 0) return -1;
    }
    for (i = 0; i < n->n_args; ++i) {
        if (args[i] != n->args[i]) break;
    }
    if (i == n->n_args) {
        *out = type;
        return 0;
    }
    built = cnet_dc_ctor(arena, n->name, args, n->n_args);
    if (built < 0) return -1;
    *out = built;
    return 0;
}

typedef struct {
    int old_var;
    int new_type;
} DcBind;

static int bind_find(const DcBind *binds, int n, int old_var) {
    int i;
    for (i = 0; i < n; ++i) {
        if (binds[i].old_var == old_var) return binds[i].new_type;
    }
    return -1;
}

static int instantiate_rec(CnetDcArena *arena, CnetDcContext *ctx, int type,
                           DcBind *binds, int *n_binds, int *out) {
    const CnetDcNode *n;
    int args[CNET_DC_MAX_ARGS];
    int i, found, fresh;
    if (out == NULL || ctx == NULL || !valid(arena, type)) return -1;
    n = &arena->nodes[type];
    if (!is_polymorphic(arena, type)) {
        *out = type;
        return 0;
    }
    if (n->kind == CNET_DC_VAR) {
        found = bind_find(binds, *n_binds, n->var);
        if (found >= 0) {
            *out = found;
            return 0;
        }
        if (*n_binds >= CNET_DC_MAX_BINDS) return -1;
        fresh = cnet_dc_var(arena, ctx->next_var);
        if (fresh < 0) return -1;
        ctx->next_var++;
        binds[*n_binds].old_var = n->var;
        binds[*n_binds].new_type = fresh;
        (*n_binds)++;
        *out = fresh;
        return 0;
    }
    if (n->n_args == 0) {
        *out = type;
        return 0;
    }
    for (i = 0; i < n->n_args; ++i) {
        if (instantiate_rec(arena, ctx, n->args[i], binds, n_binds, &args[i]) !=
            0)
            return -1;
    }
    *out = cnet_dc_ctor(arena, n->name, args, n->n_args);
    return *out < 0 ? -1 : 0;
}

int cnet_dc_instantiate(CnetDcArena *arena, CnetDcContext *ctx, int type,
                        int *out) {
    DcBind binds[CNET_DC_MAX_BINDS];
    int n_binds = 0;
    return instantiate_rec(arena, ctx, type, binds, &n_binds, out);
}

static int occurs(CnetDcArena *arena, const CnetDcContext *ctx, int var,
                  int type) {
    int applied, i;
    const CnetDcNode *n;
    if (cnet_dc_apply(arena, ctx, type, &applied) != 0) return 1;
    if (!valid(arena, applied)) return 1;
    n = &arena->nodes[applied];
    if (n->kind == CNET_DC_VAR) return n->var == var;
    for (i = 0; i < n->n_args; ++i) {
        if (occurs(arena, ctx, var, n->args[i])) return 1;
    }
    return 0;
}

static int extend(CnetDcContext *ctx, int var, int type) {
    if (ctx == NULL || ctx->subst_count >= CNET_DC_MAX_SUBST) return -1;
    ctx->subst_var[ctx->subst_count] = var;
    ctx->subst_type[ctx->subst_count] = type;
    ctx->subst_count++;
    return 0;
}

int cnet_dc_unify(CnetDcArena *arena, CnetDcContext *ctx, int t1, int t2) {
    const CnetDcNode *n1, *n2;
    int a, b, i;
    if (cnet_dc_apply(arena, ctx, t1, &a) != 0) return -1;
    if (cnet_dc_apply(arena, ctx, t2, &b) != 0) return -1;
    if (cnet_dc_equal(arena, a, b)) return 0;
    n1 = &arena->nodes[a];
    n2 = &arena->nodes[b];
    if (n1->kind == CNET_DC_VAR) {
        if (occurs(arena, ctx, n1->var, b)) return 1;
        return extend(ctx, n1->var, b) == 0 ? 0 : -1;
    }
    if (n2->kind == CNET_DC_VAR) {
        if (occurs(arena, ctx, n2->var, a)) return 1;
        return extend(ctx, n2->var, a) == 0 ? 0 : -1;
    }
    if (strcmp(n1->name, n2->name) != 0 || n1->n_args != n2->n_args) return 1;
    for (i = 0; i < n1->n_args; ++i) {
        int rc = cnet_dc_unify(arena, ctx, n1->args[i], n2->args[i]);
        if (rc != 0) return rc;
    }
    return 0;
}

int cnet_dc_can_unify(CnetDcArena *arena, int t1, int t2) {
    CnetDcContext ctx;
    int a, b;
    if (arena == NULL) return 0;
    cnet_dc_ctx_init(&ctx);
    if (cnet_dc_instantiate(arena, &ctx, t1, &a) != 0) return 0;
    if (cnet_dc_instantiate(arena, &ctx, t2, &b) != 0) return 0;
    return cnet_dc_unify(arena, &ctx, a, b) == 0 ? 1 : 0;
}

int cnet_dc_apply_fn(CnetDcArena *arena, int fn, int arg, int *result) {
    CnetDcContext ctx;
    int fn_i, arg_i, ret, arrow, rc;
    if (result == NULL) return -1;
    *result = -1;
    if (arena == NULL) return -1;
    cnet_dc_ctx_init(&ctx);
    if (cnet_dc_instantiate(arena, &ctx, fn, &fn_i) != 0) return -1;
    if (cnet_dc_instantiate(arena, &ctx, arg, &arg_i) != 0) return -1;
    ret = cnet_dc_var(arena, ctx.next_var);
    if (ret < 0) return -1;
    ctx.next_var++;
    arrow = cnet_dc_arrow(arena, arg_i, ret);
    if (arrow < 0) return -1;
    rc = cnet_dc_unify(arena, &ctx, fn_i, arrow);
    if (rc != 0) return rc == 1 ? 1 : -1;
    if (cnet_dc_apply(arena, &ctx, ret, result) != 0) return -1;
    return 0;
}

static const char *family_name(PortFamily family) {
    switch (family) {
        case PORT_RAW:
            return "raw";
        case PORT_ONEHOT:
            return "onehot";
        case PORT_BINARY_MSB:
            return "binary_msb";
        case PORT_BINARY_LSB:
            return "binary_lsb";
        case PORT_EVIDENCE:
            return "evidence";
        case PORT_CONCEPT:
            return "concept";
        default:
            return "unknown";
    }
}

static int dim_ctor(CnetDcArena *arena, char kind, size_t n) {
    char name[CNET_DC_NAME_MAX];
    snprintf(name, sizeof name, "%c%zu", kind, n);
    return cnet_dc_base(arena, name);
}

int cnet_dc_from_port(CnetDcArena *arena, Port port) {
    int args[2];
    int repr;
    args[0] = dim_ctor(arena, 'w', port.field_width);
    args[1] = dim_ctor(arena, 'c', port.field_count);
    if (args[0] < 0 || args[1] < 0) return -1;
    if (port.family == PORT_RAW) {
        int total = dim_ctor(arena, 'n', port.field_width * port.field_count);
        if (total < 0) return -1;
        return cnet_dc_ctor(arena, "raw", &total, 1);
    }
    repr = cnet_dc_ctor(arena, family_name(port.family), args, 2);
    if (repr < 0) return -1;
    if (port.tag[0] == '\0') return repr;
    return cnet_dc_ctor(arena, port.tag, &repr, 1);
}

int cnet_dc_from_contract(CnetDcArena *arena, const Contract *contract) {
    int out_ty, i, acc;
    if (arena == NULL || contract == NULL || contract->input_port_count == 0 ||
        contract->output_port_count == 0)
        return -1;
    if (contract->output_port_count == 1) {
        out_ty = cnet_dc_from_port(arena, contract->output_ports[0]);
    } else if (contract->output_port_count == 2) {
        int a = cnet_dc_from_port(arena, contract->output_ports[0]);
        int b = cnet_dc_from_port(arena, contract->output_ports[1]);
        if (a < 0 || b < 0) return -1;
        out_ty = cnet_dc_pair(arena, a, b);
    } else {
        out_ty = cnet_dc_from_port(arena, contract->output_ports[0]);
        for (i = 1; i < (int)contract->output_port_count; ++i) {
            int next = cnet_dc_from_port(arena, contract->output_ports[i]);
            if (out_ty < 0 || next < 0) return -1;
            out_ty = cnet_dc_pair(arena, out_ty, next);
        }
    }
    if (out_ty < 0) return -1;
    acc = out_ty;
    for (i = (int)contract->input_port_count - 1; i >= 0; --i) {
        int in_ty = cnet_dc_from_port(arena, contract->input_ports[i]);
        if (in_ty < 0) return -1;
        acc = cnet_dc_arrow(arena, in_ty, acc);
        if (acc < 0) return -1;
    }
    return acc;
}

int cnet_dc_ports_unify(Port producer, Port consumer) {
    CnetDcArena arena;
    int a, b;
    if (producer.tag[0] != '\0' && consumer.tag[0] != '\0' &&
        strcmp(producer.tag, consumer.tag) != 0)
        return 0;
    if (producer.family == PORT_RAW || consumer.family == PORT_RAW)
        return (producer.field_width * producer.field_count) ==
                       (consumer.field_width * consumer.field_count)
                   ? 1
                   : 0;
    cnet_dc_arena_init(&arena);
    producer.tag[0] = '\0';
    consumer.tag[0] = '\0';
    a = cnet_dc_from_port(&arena, producer);
    b = cnet_dc_from_port(&arena, consumer);
    if (a < 0 || b < 0) return 0;
    return cnet_dc_can_unify(&arena, a, b);
}

int cnet_dc_route_well_typed(const RoutePlan *plan, Port source) {
    Port cur;
    size_t i;
    if (plan == NULL || plan->length == 0) return 1;
    cur = source;
    for (i = 0; i < plan->length; ++i) {
        const BinaryTransformNetwork *btn = plan->steps[i];
        if (btn == NULL || btn->input_port_count == 0 ||
            btn->output_port_count == 0)
            return 1;
        if (!cnet_dc_ports_unify(cur, btn->input_ports[0])) return 1;
        cur = btn->output_ports[0];
    }
    return cnet_dc_ports_unify(cur, plan->goal) ? 0 : 1;
}
