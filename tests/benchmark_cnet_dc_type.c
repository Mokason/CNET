/* Type-directed enumeration bench — DreamCoder Core types on a small grammar.
   Untyped applications vs well-typed applications. Native C. */
#include "../include/cnet_dc_type.h"

#include <stdio.h>
#include <time.h>

#define N_FNS 6
#define N_ARGS 8
#define UNIFY_ITERS 20000

int main(void) {
    CnetDcArena arena;
    int t0, tint, tbool, tchar, list_int, list_bool, list_list_int, pair_ib;
    int id_fn, incr, not_fn, car, cdr, cons1;
    int fns[N_FNS], args[N_ARGS];
    int i, j, typed = 0, untyped = N_FNS * N_ARGS, result;
    int id_hits = 0, incr_hits = 0, car_hits = 0;
    clock_t t0c, t1c;
    double unify_ms;
    int u_ok = 0;

    cnet_dc_arena_init(&arena);
    t0 = cnet_dc_var(&arena, 0);
    tint = cnet_dc_base(&arena, "int");
    tbool = cnet_dc_base(&arena, "bool");
    tchar = cnet_dc_base(&arena, "char");
    list_int = cnet_dc_list(&arena, tint);
    list_bool = cnet_dc_list(&arena, tbool);
    list_list_int = cnet_dc_list(&arena, list_int);
    pair_ib = cnet_dc_pair(&arena, tint, tbool);

    id_fn = cnet_dc_arrow(&arena, t0, t0);
    incr = cnet_dc_arrow(&arena, tint, tint);
    not_fn = cnet_dc_arrow(&arena, tbool, tbool);
    car = cnet_dc_arrow(&arena, cnet_dc_list(&arena, t0), t0);
    cdr = cnet_dc_arrow(&arena, cnet_dc_list(&arena, t0),
                        cnet_dc_list(&arena, t0));
    cons1 = cnet_dc_arrow(&arena, t0, cnet_dc_arrow(&arena, cnet_dc_list(&arena, t0),
                                                   cnet_dc_list(&arena, t0)));

    fns[0] = id_fn;
    fns[1] = incr;
    fns[2] = not_fn;
    fns[3] = car;
    fns[4] = cdr;
    fns[5] = cons1;
    args[0] = tint;
    args[1] = tbool;
    args[2] = tchar;
    args[3] = list_int;
    args[4] = list_bool;
    args[5] = list_list_int;
    args[6] = pair_ib;
    args[7] = t0;

    for (i = 0; i < N_FNS; ++i) {
        for (j = 0; j < N_ARGS; ++j) {
            if (cnet_dc_apply_fn(&arena, fns[i], args[j], &result) == 0) {
                ++typed;
                if (i == 0) ++id_hits;
                if (i == 1) ++incr_hits;
                if (i == 3) ++car_hits;
            }
        }
    }

    t0c = clock();
    for (i = 0; i < UNIFY_ITERS; ++i) {
        if (cnet_dc_can_unify(&arena, list_int, list_int)) ++u_ok;
    }
    t1c = clock();
    unify_ms = 1000.0 * (double)(t1c - t0c) / (double)CLOCKS_PER_SEC;

    /* incr/not also apply to a type variable; car/cdr apply to every list
       plus a variable. cons1 is t0 -> _ so it applies to every arg. */
    if (id_hits != N_ARGS || incr_hits != 2 || car_hits != 4 ||
        typed >= untyped || typed < 20 || u_ok != UNIFY_ITERS) {
        printf("CNET_DC_TYPE_BENCH_RED typed=%d untyped=%d id=%d incr=%d car=%d\n",
               typed, untyped, id_hits, incr_hits, car_hits);
        return 1;
    }

    printf("CNET_DC_TYPE_BENCH_PASS untyped=%d typed=%d pruned=%d "
           "id_hits=%d incr_hits=%d car_hits=%d unify_iters=%d unify_ms=%.3f "
           "claim=typed_enumeration_only broader_claims=WITHHELD\n",
           untyped, typed, untyped - typed, id_hits, incr_hits, car_hits,
           UNIFY_ITERS, unify_ms);
    return 0;
}
