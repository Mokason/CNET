/* Registry name hash (Phase A): open-addressing index via registry_find.
 * make registry_hash → REGISTRY_HASH_PASS
 */
#include <stdio.h>
#include "../include/cnet_platform.h"
#include <stdlib.h>
#include <string.h>

#include "../include/nn.h"
#include "../include/router.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int make_btn(BinaryTransformNetwork *b, const char *tag) {
    Port in, out;
    memset(b, 0, sizeof *b);
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.family = PORT_ONEHOT;
    in.field_width = 4;
    in.field_count = 1;
    out = in;
    snprintf(in.tag, sizeof in.tag, "in_%s", tag);
    snprintf(out.tag, sizeof out.tag, "out_%s", tag);
    if (btn_init(b, 4, 4, 2, 8, 0.5, 1u) != 0) return -1;
    return btn_set_ports(b, in, out);
}

int main(void) {
    PrimitiveRegistry reg;
    BinaryTransformNetwork btns[64];
    char names[64][32];
    int i;

    cnet_unsetenv("CNET_REGISTRY_LINEAR");
    printf("== registry name hash ==\n");

    registry_init(&reg);
    check(reg.name_hash == NULL && reg.name_hash_cap == 0, "init: empty hash");

    /* Bulk admit unique names */
    for (i = 0; i < 48; i++) {
        snprintf(names[i], sizeof names[i], "unit_%03d", i);
        check(make_btn(&btns[i], names[i]) == 0, "btn init");
        check(registry_add(&reg, &btns[i], names[i]) == 0, "registry_add");
    }
    check(reg.count == 48, "count=48");
    check(reg.name_hash != NULL && reg.name_hash_cap >= 64, "hash table allocated");

    /* Lookups through registry_find chokepoint APIs */
    for (i = 0; i < 48; i += 7) {
        check(registry_set_state(&reg, names[i], PRIM_PROVISIONAL) == 0,
              "set_state hits hash");
        check(registry_set_shadow(&reg, names[i], "active") == 0,
              "set_shadow hits hash");
    }
    check(registry_set_state(&reg, "missing_unit", PRIM_RESET) != 0,
          "miss returns not found");

    /* Expansion attach uses registry_find */
    {
        const char *recipe[] = {"unit_000", "unit_001"};
        check(registry_set_expansion(&reg, "unit_010", recipe, 2, 100, 50, 1) == 0,
              "set_expansion via find");
        check(registry_set_expansion(&reg, "nope", recipe, 1, 1, 1, 0) != 0,
              "set_expansion miss");
    }

    /* remove_last + rebuild */
    check(registry_remove_last(&reg) == 0, "remove_last");
    check(reg.count == 47, "count after remove");
    check(registry_set_state(&reg, "unit_047", PRIM_FROZEN) != 0,
          "removed name no longer found");
    check(registry_set_state(&reg, "unit_000", PRIM_FROZEN) == 0,
          "earlier names still found");

    /* First-wins for duplicate names */
    {
        BinaryTransformNetwork d0, d1;
        check(make_btn(&d0, "dup") == 0 && make_btn(&d1, "dup") == 0, "dup btns");
        check(registry_add(&reg, &d0, "dup_name") == 0, "add dup first");
        check(registry_add(&reg, &d1, "dup_name") == 0, "add dup second");
        check(registry_set_state(&reg, "dup_name", PRIM_RESET) == 0, "find first dup");
        /* First match should be the earlier index (not last). */
        {
            size_t j;
            int first = -1;
            for (j = 0; j < reg.count; j++) {
                if (reg.entries[j].name &&
                    strcmp(reg.entries[j].name, "dup_name") == 0) {
                    first = (int)j;
                    break;
                }
            }
            check(first >= 0 && reg.entries[first].state == PRIM_RESET,
                  "first duplicate is the one mutated");
        }
        btn_free(&d0);
        btn_free(&d1);
    }

    registry_free(&reg);
    for (i = 0; i < 48; i++) btn_free(&btns[i]);
    check(1, "free ok");

    /* Linear force: no hash table; finds still work. */
    {
        PrimitiveRegistry r2;
        BinaryTransformNetwork b;
        cnet_setenv("CNET_REGISTRY_LINEAR", "1", 1);
        registry_init(&r2);
        check(make_btn(&b, "lin") == 0, "linear btn");
        check(registry_add(&r2, &b, "linear_only") == 0, "linear add");
        check(r2.name_hash == NULL, "linear mode builds no hash");
        check(registry_set_state(&r2, "linear_only", PRIM_FROZEN) == 0,
              "linear find works");
        registry_free(&r2);
        btn_free(&b);
        cnet_unsetenv("CNET_REGISTRY_LINEAR");
    }
    if (failures) {
        printf("REGISTRY_HASH_FAIL failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("REGISTRY_HASH_PASS checks=%d\n", checks);
    return 0;
}
