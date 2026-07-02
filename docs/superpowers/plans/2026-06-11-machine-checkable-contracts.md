# Machine-Checkable Contracts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Behavioral certification: contracts are named transform specs (port signature + canonical exemplar table); `btn_certify` replays them through a frozen net; planners can require certified-only plans; contracts are emitted by nn_demo and from proven plans.

**Architecture:** New `contract` module (struct + file format + certifier + emission), a `plan_table` unit extracted from `consolidate.c` (domain enumeration + strict-teacher labeling, shared by consolidation and contract emission), a `certified` flag + `require_certified` knob in the registry/planners. Spec: `docs/superpowers/specs/2026-06-11-machine-checkable-contracts-design.md`.

**Tech Stack:** C11, GCC/MinGW, make. NOT a git repo — every "commit" gate is replaced by a verification gate (build clean + tests green). `src/nn.c` must remain untouched (byte-identical weight regeneration is the regression gate).

**House rules for the executor:**
- Build/run from repo root. Build flags come from the Makefile (`-O3 -march=native -mno-avx`; `-mno-avx` is load-bearing on this toolchain).
- Test convention: `CHECK(cond, desc)` macro, `failures` counter, exit code `failures != 0` (copy the pattern from `tests/test_consolidate.c`).
- All struct fields named here are real; do not rename. Where a step says "move verbatim", the function bodies already exist in `src/consolidate.c` — relocate without behavioral edits.

---

### Task 1: Extract the shared `plan_table` unit from consolidate.c

The domain-enumeration and strict-teacher machinery currently lives as statics
in `src/consolidate.c`. Contract emission needs the same machinery. This is a
behavior-preserving refactor pinned by the existing consolidation tests and
demo output.

**Files:**
- Create: `include/plan_table.h`
- Create: `src/plan_table.c`
- Modify: `src/consolidate.c`
- Modify: `Makefile`

- [ ] **Step 1.1: Create `include/plan_table.h`** with exactly:

```c
#ifndef PLAN_TABLE_H
#define PLAN_TABLE_H

#include <stddef.h>

#include "nn.h"
#include "router.h"

/* Shared machinery for treating a proven plan as a STRICT teacher over its
   enumerated canonical input domain. Used by consolidation (distillation)
   and by contract emission. */

size_t plan_port_total(Port p);

/* Labels one enumerated input. Returns 0 (label written) or -1 (abort). */
typedef int (*PlanTeacherFn)(void *ctx, const double *in, size_t in_total,
                             double *out, size_t out_total);

/* ctx = a RoutePlan whose strict flag is already set. */
int plan_route_teacher(void *ctx, const double *in, size_t in_total,
                       double *out, size_t out_total);

typedef struct {
    DagPlan teacher;          /* borrowed root, strict = 1 */
    const DagSource *declared;
    size_t n_sources;
    const size_t *offsets;    /* slice of the flat input vector per source */
} PlanDagTeacherCtx;

int plan_dag_teacher(void *ctx, const double *in, size_t in_total,
                     double *out, size_t out_total);

/* Collect every DAG_PRIMITIVE in the tree (duplicates included). Returns the
   count, writing up to cap entries. */
size_t plan_dag_collect_members(const DagNode *node,
                                BinaryTransformNetwork **members,
                                size_t cap, size_t n);

/* Count how often each source index feeds the tree. Returns 0, or -1 on an
   index outside [0, n_sources). */
int plan_dag_count_sources(const DagNode *node, size_t *counts,
                           size_t n_sources);

/* The teacher-labeled table over a plan's full canonical input domain. */
typedef struct {
    double *inputs;    /* kept x in_total */
    double *targets;   /* kept x out_total */
    size_t kept;       /* inputs the strict teacher labeled */
    size_t aborts;     /* inputs the strict teacher refused (excluded) */
    size_t in_total;
    size_t out_total;
} PlanTable;

/* Enumerate the canonical domain of in_ports (deterministic order: ports
   left to right, later fields fastest), refuse RAW fields, overflow, or
   more than max_samples combos; label every input with the strict teacher;
   snapshot and restore the distinct members' reliability counters around
   the sweep (a teacher sweep is not deployment experience). Returns 0 with
   *out filled (kept may be 0 -- caller decides), or -1 on refusal or
   allocation failure (*out zeroed). */
int plan_table_build(
    const Port *in_ports, size_t n_in,
    size_t out_total,
    PlanTeacherFn teacher, void *ctx,
    BinaryTransformNetwork *const *members, size_t n_members,
    size_t max_samples,
    PlanTable *out);

void plan_table_free(PlanTable *t);

#endif
```

- [ ] **Step 1.2: Create `src/plan_table.c`.** Move these from
`src/consolidate.c` VERBATIM (bodies unchanged), renaming per this table; the
`Domain`/`StatSnap` helpers stay `static` inside plan_table.c:

| consolidate.c (current) | plan_table.c (new) | linkage |
|---|---|---|
| `port_total` | `plan_port_total` | exported |
| `field_cardinality` | `field_cardinality` | static |
| `field_write` | `field_write` | static |
| `Domain` + `domain_build/write/free` | same names | static |
| `StatSnap` + `snap_members/restore_members` | same names | static |
| `route_teacher` | `plan_route_teacher` | exported |
| `DagTeacherCtx` | `PlanDagTeacherCtx` (in header) | — |
| `dag_teacher` | `plan_dag_teacher` | exported |
| `dag_collect_members` | `plan_dag_collect_members` | exported |
| `dag_count_sources` | `plan_dag_count_sources` | exported |

File head: `#include "../include/plan_table.h"`, `#include <stdlib.h>`,
`#include <string.h>`. Then add the new composite function (this is the only
new logic — it is the teacher-pass section currently inlined in
`consolidate_core`, lifted out):

```c
int plan_table_build(
    const Port *in_ports, size_t n_in,
    size_t out_total,
    PlanTeacherFn teacher, void *ctx,
    BinaryTransformNetwork *const *members, size_t n_members,
    size_t max_samples,
    PlanTable *out
) {
    Domain dom;
    StatSnap *snaps = NULL;
    size_t n_snaps = 0;
    size_t s;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (in_ports == NULL || n_in == 0 || out_total == 0 || teacher == NULL) {
        return -1;
    }
    if (domain_build(in_ports, n_in, &dom) != 0) {
        return -1;
    }
    if (dom.combos > max_samples) {
        domain_free(&dom);
        return -1;
    }

    snaps = malloc((n_members > 0 ? n_members : 1) * sizeof *snaps);
    out->inputs = malloc(dom.combos * dom.in_total * sizeof *out->inputs);
    out->targets = malloc(dom.combos * out_total * sizeof *out->targets);
    if (snaps == NULL || out->inputs == NULL || out->targets == NULL) {
        free(snaps);
        plan_table_free(out);
        domain_free(&dom);
        return -1;
    }

    n_snaps = snap_members(members, n_members, snaps);
    for (s = 0; s < dom.combos; ++s) {
        double *in_vec = out->inputs + out->kept * dom.in_total;
        double *out_vec = out->targets + out->kept * out_total;

        domain_write(&dom, s, in_vec);
        if (teacher(ctx, in_vec, dom.in_total, out_vec, out_total) == 0) {
            ++out->kept;
        } else {
            ++out->aborts;
        }
    }
    restore_members(snaps, n_snaps);

    out->in_total = dom.in_total;
    out->out_total = out_total;
    free(snaps);
    domain_free(&dom);
    return 0;
}

void plan_table_free(PlanTable *t) {
    if (t == NULL) {
        return;
    }
    free(t->inputs);
    free(t->targets);
    memset(t, 0, sizeof *t);
}
```

- [ ] **Step 1.3: Rewrite `src/consolidate.c` to use the unit.** Delete every
moved function. `#include "../include/plan_table.h"`. Replace `consolidate_core`'s
domain/snapshot/teacher-loop section with `plan_table_build`; train/verify/seed
on `table.inputs/targets/kept`. The reworked core (replace the whole function):

```c
static int consolidate_core(
    BinaryTransformNetwork *const *members,
    size_t n_members,
    const Port *in_ports,
    size_t n_in,
    Port out_port,
    PlanTeacherFn teacher,
    void *ctx,
    const ConsolidateConfig *cfg_opt,
    BinaryTransformNetwork *out_student,
    ConsolidateReport *report
) {
    ConsolidateConfig cfg;
    ConsolidateReport rep;
    PlanTable table;
    double *clean = NULL;
    BinaryTransformNetwork student;
    size_t out_total = plan_port_total(out_port);
    size_t s;
    int rc = -1;

    if (cfg_opt != NULL) {
        cfg = *cfg_opt;
    } else {
        consolidate_config_defaults(&cfg);
    }
    memset(&rep, 0, sizeof rep);
    memset(&student, 0, sizeof student);
    memset(&table, 0, sizeof table);

    if (plan_table_build(in_ports, n_in, out_total, teacher, ctx,
                         members, n_members, cfg.max_samples, &table) != 0) {
        goto done;
    }
    rep.samples = table.kept;
    rep.teacher_aborts = table.aborts;
    if (table.kept == 0) {
        goto done;
    }

    clean = malloc(out_total * sizeof *clean);
    if (clean == NULL) {
        goto done;
    }

    /* initial_hidden 0 = auto. A student must NOT start from the usual
       1-neuron seed: during the first growth windows every input projects
       through that bottleneck, the output layer saturates on a low-rank
       approximation, and squared-error sigmoid gradients (~sigma' at the
       rails) are too weak to repair the confidently-wrong bits afterwards
       -- seed-dependent refusals in practice. Starting at the task's width
       skips the bottleneck era entirely (measured: 24/24 seeds verify vs
       ~1/8 from a 1-neuron start). */
    {
        size_t initial = cfg.initial_hidden;

        if (initial == 0) {
            initial = table.in_total > out_total ? table.in_total : out_total;
            if (initial > cfg.max_hidden) {
                initial = cfg.max_hidden;
            }
        }
        if (btn_init(&student, table.in_total, out_total, initial,
                     cfg.max_hidden, cfg.learning_rate, cfg.seed) != 0) {
            goto done;
        }
    }
    if (btn_set_io_ports(&student, in_ports, n_in, &out_port, 1) != 0) {
        btn_free(&student);
        goto done;
    }

    rep.final_loss = btn_train_dynamic(&student, table.inputs, table.targets,
                                       table.kept, cfg.max_epochs,
                                       cfg.growth_window, cfg.target_loss,
                                       cfg.min_improvement);

    /* Verification: the student alone must reproduce the teacher, and its
       RAW output must be in-domain -- the same bar the executors score, so
       the seeded counters mean what deployment counters mean. */
    for (s = 0; s < table.kept; ++s) {
        const double *raw = btn_forward(&student,
                                        table.inputs + s * table.in_total);
        const double *want = table.targets + s * out_total;
        int ok = raw != NULL && port_validate(out_port, raw);

        if (ok && port_canonicalize(out_port, raw, clean) == 0) {
            size_t i;
            for (i = 0; i < out_total; ++i) {
                if (clean[i] != want[i]) {
                    ok = 0;
                    break;
                }
            }
        } else {
            ok = 0;
        }
        if (ok) {
            ++rep.verified;
        } else {
            ++rep.missed;
        }
    }

    if ((double)rep.verified < cfg.min_verify_rate * (double)table.kept) {
        btn_free(&student);
        goto done;
    }

    student.output_successes = rep.verified;
    student.output_failures = rep.missed;
    *out_student = student;
    rc = 0;

done:
    if (report != NULL) {
        *report = rep;
    }
    free(clean);
    plan_table_free(&table);
    return rc;
}
```

`consolidate_route` keeps its guards and now passes `plan_route_teacher`;
`consolidate_dag` keeps its guards, uses `plan_dag_count_sources` /
`plan_dag_collect_members` and `PlanDagTeacherCtx` + `plan_dag_teacher`.
(Only identifier renames per the Step 1.2 table — logic unchanged.)
Note `consolidate_core`'s cap check is gone: `plan_table_build` owns it.

- [ ] **Step 1.4: Makefile.** Add `PLAN_TABLE := src/plan_table.c` next to
`CONSOLIDATE`; add `$(PLAN_TABLE)` and `include/plan_table.h` to the
`test_consolidate` and `chunk_demo` rules (compile line gains
`$(PLAN_TABLE)`).

- [ ] **Step 1.5: Verify the refactor is invisible.**

Run: `make test_consolidate; ./test_consolidate.exe`
Expected: `All consolidation tests passed.`

Run: `make test`
Expected: all six suites pass.

Run: `make chunk_demo; ./chunk_demo.exe`
Expected: CHUNK PASS, with the same verified counts as before
(16/16, 256/256, 256/256) and identical plan trees.

---

### Task 2: Contract struct, file format, save/load/free (TDD)

**Files:**
- Create: `include/contract.h`
- Create: `src/contract.c`
- Create: `tests/test_certify.c`
- Modify: `Makefile`

- [ ] **Step 2.1: Create `include/contract.h`** with exactly:

```c
#ifndef CONTRACT_H
#define CONTRACT_H

#include <stddef.h>

#include "nn.h"
#include "router.h"

/* A machine-checkable contract: a NAMED transform defined by data -- a full
   port signature plus an exemplar table over canonical values. Primitives
   CLAIM a contract; btn_certify replays the exemplars through the frozen
   net and grants or denies. Certification runs on demand and is never
   persisted (a replay is cheap; a certificate file would reintroduce
   staleness). */

#define CONTRACT_NAME_MAX 64  /* atom over [A-Za-z0-9_], like port tags */

typedef struct {
    char name[CONTRACT_NAME_MAX];
    Port input_ports[BTN_MAX_INPUT_PORTS];
    size_t input_port_count;
    Port output_ports[BTN_MAX_OUTPUT_PORTS];
    size_t output_port_count;
    double *inputs;        /* exemplar_count x (sum of input totals) */
    double *outputs;       /* exemplar_count x (sum of output totals) */
    size_t exemplar_count;
    int owns_data;         /* nonzero -> contract_free releases the tables */
} Contract;

/* Authoring: fill *c from a primitive's port signature and a BORROWED
   exemplar table (typically the training data). c->owns_data = 0; do not
   contract_free the tables' owner before saving. Values must be canonical.
   Returns 0, or -1 on a bad name/empty table. */
int contract_init_borrowed(Contract *c, const char *name,
                           const BinaryTransformNetwork *btn,
                           const double *inputs, const double *targets,
                           size_t exemplar_count);

/* Persist / restore ("CNET_CONTRACT 1"). contract_load allocates owned
   tables (owns_data = 1) and validates: known families, name and tag
   atoms, every value exactly 0.0 or 1.0 and every port slice canonical
   (port_validate). Returns 0, or -1 on malformed input (*c untouched). */
int contract_save(const Contract *c, const char *path);
int contract_load(Contract *c, const char *path);
void contract_free(Contract *c);

typedef struct {
    size_t exemplars;
    size_t passed;
    size_t failed;
} CertifyReport;

/* Certification: (1) SIGNATURE -- the primitive's ports must match the
   contract's exactly: count, family, field_width, field_count AND tag
   (certification is what entitles a primitive to wear the contract's
   tags). (2) BEHAVIOR -- every exemplar replays through the frozen net;
   the raw output must be in-domain on EVERY output port and canonicalize
   to the exemplar's output exactly. All exemplars must pass. Stateless;
   reliability counters are not touched. Returns 0 (certified) or -1;
   report (optional) carries counts either way. */
int btn_certify(BinaryTransformNetwork *btn, const Contract *c,
                CertifyReport *report);

/* Certify-then-register: refuses registration entirely (-1, nothing
   added) when btn_certify fails; on success the entry's certified flag
   is set. */
int registry_add_certified(PrimitiveRegistry *reg,
                           BinaryTransformNetwork *btn,
                           const char *name, const Contract *c);

/* Emission: replay a proven plan as a STRICT teacher over its enumerated
   canonical domain (same guards and member-stats hygiene as
   consolidation; teacher-aborted inputs are excluded) and build the
   contract of the composite behavior. Unlike consolidation a 1-step /
   1-primitive plan is allowed. Output tables are owned (owns_data = 1).
   Returns 0, or -1 on refusal (RAW or over-cap domain, no labeled rows,
   bad name, unconsumed DAG source). */
int contract_from_route(const RoutePlan *plan, const char *name,
                        size_t max_samples, Contract *out);
int contract_from_dag(const DagPlan *plan, const DagSource *sources,
                      size_t n_sources, const char *name,
                      size_t max_samples, Contract *out);

#endif
```

- [ ] **Step 2.2: Stub `src/contract.c`** so the test target links: every
function returns -1 (and `contract_free` does nothing except guard NULL);
include `../include/contract.h`, `../include/plan_table.h`, `<stdio.h>`,
`<stdlib.h>`, `<string.h>`.

- [ ] **Step 2.3: Write `tests/test_certify.c`** — first slice: round-trip +
malformed-file tests. Copy the harness conventions from
`tests/test_consolidate.c` (CHECK macro, `PT`, `msb2`, `make_decoder` —
reproduce those helpers verbatim; decoder = ONEHOT4 "sym" -> BINARY_MSB2
"val", i -> i, trained in-test). Tests:

```c
static void test_roundtrip(void) {
    BinaryTransformNetwork decoder = {0};
    Contract c, back;
    double inputs[4][4] = {{0}};
    double targets[4][2];
    int i;

    printf("contract round-trip:\n");
    CHECK(make_decoder(&decoder) == 0, "decoder trains");
    for (i = 0; i < 4; ++i) {
        inputs[i][i] = 1.0;
        msb2(i, targets[i]);
    }
    CHECK(contract_init_borrowed(&c, "decode_sym", &decoder,
                                 &inputs[0][0], &targets[0][0], 4) == 0,
          "init from a borrowed table");
    CHECK(c.owns_data == 0 && c.exemplar_count == 4 &&
          c.input_port_count == 1 && c.output_port_count == 1 &&
          strcmp(c.input_ports[0].tag, "sym") == 0,
          "signature copied from the primitive");
    CHECK(contract_init_borrowed(&c, "bad name!", &decoder,
                                 &inputs[0][0], &targets[0][0], 4) == -1,
          "rejects a non-atom name");

    CHECK(contract_init_borrowed(&c, "decode_sym", &decoder,
                                 &inputs[0][0], &targets[0][0], 4) == 0 &&
          contract_save(&c, "tmp_contract.txt") == 0,
          "saves");
    memset(&back, 0, sizeof back);
    CHECK(contract_load(&back, "tmp_contract.txt") == 0,
          "loads");
    CHECK(back.owns_data == 1 && back.exemplar_count == 4 &&
          strcmp(back.name, "decode_sym") == 0 &&
          back.input_ports[0].family == PORT_ONEHOT &&
          back.input_ports[0].field_width == 4 &&
          strcmp(back.input_ports[0].tag, "sym") == 0 &&
          strcmp(back.output_ports[0].tag, "val") == 0,
          "round-trip preserves name, ports, tags");
    {
        int same = 1;
        for (i = 0; i < 4 * 4; ++i) {
            if (back.inputs[i] != (&inputs[0][0])[i]) same = 0;
        }
        for (i = 0; i < 4 * 2; ++i) {
            if (back.outputs[i] != (&targets[0][0])[i]) same = 0;
        }
        CHECK(same, "round-trip preserves the exemplar values");
    }
    contract_free(&back);

    /* malformed files refuse */
    {
        FILE *f = fopen("tmp_contract_bad.txt", "w");
        if (f != NULL) {
            fputs("CNET_WRONG 1\n", f);
            fclose(f);
        }
        memset(&back, 0, sizeof back);
        CHECK(contract_load(&back, "tmp_contract_bad.txt") == -1,
              "rejects a wrong magic");
    }
    {
        /* valid header, non-canonical exemplar value */
        FILE *f = fopen("tmp_contract_bad.txt", "w");
        if (f != NULL) {
            fputs("CNET_CONTRACT 1\ndecode_sym\nINPUTS 1\n"
                  "PORT_IN onehot 4 1 sym\nOUTPUTS 1\n"
                  "PORT_OUT binary_msb 2 1 val\nEXEMPLARS 1\n"
                  "1 0 0 0 0.5 0\n", f);
            fclose(f);
        }
        memset(&back, 0, sizeof back);
        CHECK(contract_load(&back, "tmp_contract_bad.txt") == -1,
              "rejects a non-canonical exemplar value");
    }
    CHECK(contract_load(&back, "no_such_file.txt") == -1,
          "missing file returns -1");

    remove("tmp_contract.txt");
    remove("tmp_contract_bad.txt");
    btn_free(&decoder);
}
```

`main()` calls `test_roundtrip()` for now; summary print + exit code as in
test_consolidate.

- [ ] **Step 2.4: Makefile target** (and add `test_certify` to the `test`
list and both spellings to `clean`):

```make
CONTRACT := src/contract.c
CERTIFY_TEST := tests/test_certify.c

test_certify: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(CERTIFY_TEST) include/nn.h include/router.h include/plan_table.h include/consolidate.h include/contract.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(CERTIFY_TEST) $(LDFLAGS)
```

- [ ] **Step 2.5: Watch it fail.**
Run: `make test_certify; ./test_certify.exe`
Expected: the positive round-trip CHECKs FAIL (stubs return -1); the
malformed-file CHECKs pass vacuously for now.

- [ ] **Step 2.6: Implement** `contract_init_borrowed`, `contract_save`,
`contract_load`, `contract_free` in `src/contract.c`:
- Local statics: `family_token(PortFamily)` returning
  `"raw"/"onehot"/"binary_msb"/"binary_lsb"`, `family_parse(const char*)`
  returning -1 on unknown; `name_valid(const char*)` (nonempty, <
  CONTRACT_NAME_MAX, chars in `[A-Za-z0-9_]`); `ports_total(const Port*,
  size_t)` summing `plan_port_total`.
- `contract_init_borrowed`: validate name + non-NULL tables + k > 0; copy
  name and both port arrays/counts from the btn; point `inputs/outputs` at
  the borrowed tables (cast away const); `owns_data = 0`.
- `contract_save`: write the spec format exactly — magic line, name line,
  `INPUTS n`, one `PORT_IN family width count tag|-` line per port (write
  `-` when `tag[0] == '\0'`), same for OUTPUTS, `EXEMPLARS k`, then k lines
  of `in_total + out_total` values printed with `%g` separated by single
  spaces. Return -1 on any fopen/write failure.
- `contract_load`: parse into a zeroed local, then validate before
  committing: magic + version 1; atom name; port counts in
  `[1, BTN_MAX_*]`; known families; tags via `port_set_tag` (rejects bad
  atoms; `-` means untagged); every exemplar value `== 0.0 || == 1.0`; and
  every port slice of every row passes `port_validate`. Allocate owned
  tables; on ANY failure free them and return -1 leaving *c untouched;
  on success `*c = local` with `owns_data = 1`.
- `contract_free`: if `owns_data`, free both tables; then zero the struct.

- [ ] **Step 2.7: Verify green.**
Run: `make test_certify; ./test_certify.exe`
Expected: all round-trip CHECKs pass.

---

### Task 3: `btn_certify` (TDD)

**Files:**
- Modify: `src/contract.c`
- Modify: `tests/test_certify.c`

- [ ] **Step 3.1: Add the certification tests** (new function
`test_certification`, called from main):

```c
static void test_certification(void) {
    BinaryTransformNetwork decoder = {0};
    Contract c;
    CertifyReport rep;
    double inputs[4][4] = {{0}};
    double targets[4][2];
    int i;

    printf("btn_certify:\n");
    CHECK(make_decoder(&decoder) == 0, "decoder trains");
    for (i = 0; i < 4; ++i) {
        inputs[i][i] = 1.0;
        msb2(i, targets[i]);
    }
    CHECK(contract_init_borrowed(&c, "decode_sym", &decoder,
                                 &inputs[0][0], &targets[0][0], 4) == 0,
          "contract built");

    decoder.output_successes = 7;
    decoder.output_failures = 3;
    memset(&rep, 0, sizeof rep);
    CHECK(btn_certify(&decoder, &c, &rep) == 0,
          "a correct net certifies");
    CHECK(rep.exemplars == 4 && rep.passed == 4 && rep.failed == 0,
          "report counts the full pass");
    CHECK(decoder.output_successes == 7 && decoder.output_failures == 3,
          "certification leaves reliability counters untouched");

    /* behavior gate: one tampered exemplar fails the whole claim */
    targets[2][0] = 1.0 - targets[2][0];
    memset(&rep, 0, sizeof rep);
    CHECK(btn_certify(&decoder, &c, &rep) == -1,
          "a tampered exemplar denies certification");
    CHECK(rep.failed >= 1 && rep.passed + rep.failed == 4,
          "report shows the miss");
    targets[2][0] = 1.0 - targets[2][0];

    /* signature gate: tags and shape must match EXACTLY */
    {
        Contract wrong = c;
        CHECK(port_set_tag(&wrong.input_ports[0], "other") == 0 &&
              btn_certify(&decoder, &wrong, NULL) == -1,
              "a different tag denies certification");
        wrong = c;
        CHECK(port_set_tag(&wrong.input_ports[0], "") == 0 &&
              btn_certify(&decoder, &wrong, NULL) == -1,
              "an untagged contract port vs a tagged primitive denies");
        wrong = c;
        wrong.input_ports[0].field_width = 5;
        CHECK(btn_certify(&decoder, &wrong, NULL) == -1,
              "a shape mismatch denies certification");
    }

    btn_free(&decoder);
}
```

- [ ] **Step 3.2: Watch it fail.**
Run: `make test_certify; ./test_certify.exe`
Expected: "a correct net certifies" and the report CHECKs FAIL (stub);
the deny-CHECKs pass vacuously until implementation.

- [ ] **Step 3.3: Implement `btn_certify`:**

```c
static int ports_equal(const Port *a, size_t na, const Port *b, size_t nb) {
    size_t i;

    if (na != nb) {
        return 0;
    }
    for (i = 0; i < na; ++i) {
        if (a[i].family != b[i].family ||
            a[i].field_width != b[i].field_width ||
            a[i].field_count != b[i].field_count ||
            strcmp(a[i].tag, b[i].tag) != 0) {
            return 0;
        }
    }
    return 1;
}

int btn_certify(BinaryTransformNetwork *btn, const Contract *c,
                CertifyReport *report) {
    size_t in_total, out_total;
    double *clean = NULL;
    size_t s;
    int rc;

    if (report != NULL) {
        memset(report, 0, sizeof *report);
    }
    if (btn == NULL || c == NULL || c->exemplar_count == 0) {
        return -1;
    }

    /* Gate 1: the claim is specific -- exact signature including tags. */
    if (!ports_equal(btn->input_ports, btn->input_port_count,
                     c->input_ports, c->input_port_count) ||
        !ports_equal(btn->output_ports, btn->output_port_count,
                     c->output_ports, c->output_port_count)) {
        return -1;
    }

    in_total = ports_total(c->input_ports, c->input_port_count);
    out_total = ports_total(c->output_ports, c->output_port_count);
    clean = malloc(out_total * sizeof *clean);
    if (clean == NULL) {
        return -1;
    }
    if (report != NULL) {
        report->exemplars = c->exemplar_count;
    }

    /* Gate 2: every exemplar must replay exactly. */
    for (s = 0; s < c->exemplar_count; ++s) {
        const double *raw = btn_forward(btn, c->inputs + s * in_total);
        const double *want = c->outputs + s * out_total;
        int ok = raw != NULL;
        size_t off = 0;
        size_t p, i;

        for (p = 0; ok && p < c->output_port_count; ++p) {
            size_t total = plan_port_total(c->output_ports[p]);

            if (!port_validate(c->output_ports[p], raw + off) ||
                port_canonicalize(c->output_ports[p], raw + off,
                                  clean + off) != 0) {
                ok = 0;
            }
            off += total;
        }
        for (i = 0; ok && i < out_total; ++i) {
            if (clean[i] != want[i]) {
                ok = 0;
            }
        }
        if (report != NULL) {
            if (ok) {
                ++report->passed;
            } else {
                ++report->failed;
            }
        }
        if (!ok && report == NULL) {
            break;  /* no report wanted: first miss decides */
        }
    }

    rc = (report != NULL)
             ? (report->failed == 0 ? 0 : -1)
             : (s == c->exemplar_count ? 0 : -1);
    free(clean);
    return rc;
}
```

- [ ] **Step 3.4: Verify green.**
Run: `make test_certify; ./test_certify.exe`
Expected: all certification CHECKs pass (including the deny cases, now
exercised against real logic).

---

### Task 4: Registry flag + `require_certified` planner knob (TDD)

**Files:**
- Modify: `include/router.h`
- Modify: `src/router.c`
- Modify: `src/contract.c`
- Modify: `tests/test_certify.c`

- [ ] **Step 4.1: Add the fields** in `include/router.h`:
`RegistryEntry` gains `int certified;  /* set only by registry_add_certified */`
after `name`; `PrimitiveRegistry` gains, after `capacity`:

```c
    /* Planning policy: nonzero -> route_plan/dag_plan consider ONLY
       certified entries, so a returned plan is certified end-to-end.
       registry_init zeroes it -- opt in before planning (mirrors the
       strict-execution precedent: zero-init = legacy behavior). */
    int require_certified;
```

In `src/router.c`: `registry_init` sets `reg->require_certified = 0;`
explicitly; `registry_add` sets the new entry's `certified = 0`.

Run: `make test` — everything still green (fields are inert).

- [ ] **Step 4.2: Write the failing tests** (new function
`test_registry_and_knob`; needs a second trained net — add `make_decoder_b`,
identical to `make_decoder` but seed `37u`, so it certifies against the same
contract):

```c
static void test_registry_and_knob(void) {
    BinaryTransformNetwork decoder = {0};
    BinaryTransformNetwork decoder_b = {0};
    BinaryTransformNetwork inc = {0};
    Contract c;
    PrimitiveRegistry reg;
    RoutePlan plan;
    double inputs[4][4] = {{0}};
    double targets[4][2];
    int i;

    printf("registry_add_certified + require_certified:\n");
    CHECK(make_decoder(&decoder) == 0 && make_decoder_b(&decoder_b) == 0 &&
          make_inc(&inc, 1) == 0,
          "primitives train");
    for (i = 0; i < 4; ++i) {
        inputs[i][i] = 1.0;
        msb2(i, targets[i]);
    }
    CHECK(contract_init_borrowed(&c, "decode_sym", &decoder,
                                 &inputs[0][0], &targets[0][0], 4) == 0,
          "contract built");

    registry_init(&reg);
    CHECK(reg.require_certified == 0, "knob zero-initialized");

    /* the imposter: inc claims the decoder contract -- refused, nothing
       registered */
    CHECK(registry_add_certified(&reg, &inc, "imposter", &c) == -1 &&
          reg.count == 0,
          "an imposter is refused and not registered");

    CHECK(registry_add_certified(&reg, &decoder, "decoder", &c) == 0 &&
          reg.count == 1 && reg.entries[0].certified == 1,
          "a certified net registers with the flag set");

    /* decoy: same contract-compatible behavior, registered PLAIN, with
       better evidence -- wins on score until the knob filters it */
    registry_add(&reg, &decoder_b, "decoy");
    CHECK(reg.entries[1].certified == 0, "plain registration stays uncertified");
    decoder_b.output_successes = 50;
    decoder_b.output_failures = 0;

    CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, "sym"),
                     PT(PORT_BINARY_MSB, 2, 1, "val"), &plan) == 0 &&
          plan.length == 1 && plan.steps[0] == &decoder_b,
          "knob off: the better-evidenced uncertified decoy wins");

    reg.require_certified = 1;
    CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, "sym"),
                     PT(PORT_BINARY_MSB, 2, 1, "val"), &plan) == 0 &&
          plan.length == 1 && plan.steps[0] == &decoder,
          "knob on: only the certified entry is considered");

    /* an uncertified-only registry yields no plan under the knob */
    {
        PrimitiveRegistry reg2;
        registry_init(&reg2);
        registry_add(&reg2, &decoder_b, "decoy");
        reg2.require_certified = 1;
        CHECK(route_plan(&reg2, PT(PORT_ONEHOT, 4, 1, "sym"),
                         PT(PORT_BINARY_MSB, 2, 1, "val"), &plan) == -1,
              "knob on + nothing certified -> no plan");
        registry_free(&reg2);
    }

    /* dag planner honors the knob too */
    {
        PrimitiveRegistry reg3;
        DagSource src;
        DagPlan dplan = {0};
        double dummy[4] = {1.0, 0.0, 0.0, 0.0};

        registry_init(&reg3);
        registry_add(&reg3, &decoder, "decoder");  /* plain -> uncertified */
        src.type = PT(PORT_ONEHOT, 4, 1, "sym");
        src.values = dummy;
        reg3.require_certified = 1;
        CHECK(dag_plan(&reg3, &src, 1, PT(PORT_BINARY_MSB, 2, 1, "val"),
                       &dplan) == -1,
              "dag knob on + nothing certified -> no plan");
        reg3.require_certified = 0;
        CHECK(dag_plan(&reg3, &src, 1, PT(PORT_BINARY_MSB, 2, 1, "val"),
                       &dplan) == 0,
              "dag knob off: plan found");
        dag_free(&dplan);
        registry_free(&reg3);
    }

    registry_free(&reg);
    btn_free(&decoder);
    btn_free(&decoder_b);
    btn_free(&inc);
}
```

(`make_inc` is the same helper as in `tests/test_consolidate.c` — copy it
verbatim, including the `trained` parameter.)

- [ ] **Step 4.3: Watch it fail.**
Run: `make test_certify; ./test_certify.exe`
Expected: FAILs on "an imposter is refused" (stub returns -1 BUT count check
passes — the failing ones are "a certified net registers" and both knob-on
CHECKs, since the knob is not yet read by the planners).

- [ ] **Step 4.4: Implement.**
In `src/contract.c`:

```c
int registry_add_certified(PrimitiveRegistry *reg,
                           BinaryTransformNetwork *btn,
                           const char *name, const Contract *c) {
    if (reg == NULL || btn == NULL || name == NULL || c == NULL) {
        return -1;
    }
    if (btn_certify(btn, c, NULL) != 0) {
        return -1;
    }
    if (registry_add(reg, btn, name) != 0) {
        return -1;
    }
    reg->entries[reg->count - 1].certified = 1;
    return 0;
}
```

In `src/router.c`: add near the top

```c
/* require_certified gate: when the registry demands certification, an
   uncertified entry is invisible to the PLANNERS (executors never read
   the registry). */
static int entry_usable(const PrimitiveRegistry *reg, size_t i) {
    return !reg->require_certified || reg->entries[i].certified;
}
```

then guard EVERY planner-side iteration over `reg->entries` with
`if (!entry_usable(reg, i)) continue;` as the first line of the loop body.
Find the sites with: `grep -n "entries\[" src/router.c` — they are (a) the
reachable-type enumeration loop and (b) the DP edge-relaxation loop inside
`route_plan`, and (c) the ranked-alternatives construction used by
`dag_plan` (`rank_by_reliability`; pass the registry through if it
currently receives only the entries array). Executors and registry
add/free functions are NOT touched.

- [ ] **Step 4.5: Verify green + no regression.**
Run: `make test_certify; ./test_certify.exe` — all pass.
Run: `make test` — all suites pass (knob off everywhere = old behavior).

---

### Task 5: Contract emission from plans (TDD)

**Files:**
- Modify: `src/contract.c`
- Modify: `tests/test_certify.c`

- [ ] **Step 5.1: Write the failing tests** (new function `test_emission`;
reuse `make_decoder`, `make_inc`, plus `make_decoder2`/`make_combiner`
copied verbatim from `tests/test_consolidate.c`):

```c
static void test_emission(void) {
    BinaryTransformNetwork decoder = {0};
    BinaryTransformNetwork inc = {0};
    PrimitiveRegistry reg;
    RoutePlan plan;
    Contract c;
    int i;

    printf("contract_from_route / contract_from_dag:\n");
    CHECK(make_decoder(&decoder) == 0 && make_inc(&inc, 1) == 0,
          "primitives train");
    registry_init(&reg);
    registry_add(&reg, &decoder, "decoder");
    registry_add(&reg, &inc, "inc");

    CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, "sym"),
                     PT(PORT_BINARY_MSB, 2, 1, "val2"), &plan) == 0 &&
          plan.length == 2,
          "teacher chain plans");

    decoder.output_successes = 7;
    decoder.output_failures = 3;
    memset(&c, 0, sizeof c);
    CHECK(contract_from_route(&plan, "sym_inc", 4096, &c) == 0,
          "route emission succeeds");
    CHECK(c.owns_data == 1 && c.exemplar_count == 4 &&
          strcmp(c.name, "sym_inc") == 0 &&
          strcmp(c.input_ports[0].tag, "sym") == 0 &&
          strcmp(c.output_ports[0].tag, "val2") == 0,
          "emitted signature is the plan boundary (incl tags)");
    CHECK(decoder.output_successes == 7 && decoder.output_failures == 3,
          "member evidence restored after emission");
    {
        /* the table IS the teacher: row i = onehot(i) -> bits((i+1)%4) */
        int all = 1;
        for (i = 0; i < 4; ++i) {
            double want[2];
            int hot = -1, j;
            for (j = 0; j < 4; ++j) {
                if (c.inputs[(size_t)i * 4 + j] == 1.0) hot = j;
            }
            msb2((hot + 1) % 4, want);
            if (c.outputs[(size_t)i * 2] != want[0] ||
                c.outputs[(size_t)i * 2 + 1] != want[1]) all = 0;
        }
        CHECK(all, "emitted rows match the teacher's mapping");
    }
    contract_free(&c);

    /* a 1-step plan is allowed for emission (unlike consolidation) */
    CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, "sym"),
                     PT(PORT_BINARY_MSB, 2, 1, "val"), &plan) == 0 &&
          plan.length == 1 &&
          contract_from_route(&plan, "decode_sym", 4096, &c) == 0 &&
          c.exemplar_count == 4,
          "1-step emission allowed");
    /* ...and the original primitive certifies against its own emitted
       contract */
    CHECK(btn_certify(&decoder, &c, NULL) == 0,
          "primitive certifies against its own emitted contract");
    contract_free(&c);

    /* guards: cap */
    CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, "sym"),
                     PT(PORT_BINARY_MSB, 2, 1, "val2"), &plan) == 0 &&
          contract_from_route(&plan, "sym_inc", 3, &c) == -1,
          "refuses when the domain exceeds max_samples");
    CHECK(contract_from_route(&plan, "bad name!", 4096, &c) == -1,
          "refuses a non-atom name");

    registry_free(&reg);
    btn_free(&decoder);
    btn_free(&inc);
}

static void test_emission_certifies_chunk(void) {
    BinaryTransformNetwork decoder2 = {0};
    BinaryTransformNetwork combiner = {0};
    BinaryTransformNetwork chunk = {0};
    PrimitiveRegistry reg;
    DagSource sources[2];
    DagPlan plan = {0};
    Contract c;
    double dummy_a[2] = {1.0, 0.0};
    double dummy_b[2] = {0.0, 1.0};

    printf("emission certifies the chunk (the loop closes):\n");
    CHECK(make_decoder2(&decoder2) == 0 && make_combiner(&combiner) == 0,
          "primitives train");
    registry_init(&reg);
    registry_add(&reg, &decoder2, "decoder2");
    registry_add(&reg, &combiner, "combiner");
    sources[0].type = PT(PORT_ONEHOT, 2, 1, "bsym");
    sources[0].values = dummy_a;
    sources[1].type = PT(PORT_ONEHOT, 2, 1, "bsym");
    sources[1].values = dummy_b;
    CHECK(dag_plan(&reg, sources, 2, PT(PORT_BINARY_MSB, 2, 1, "pair"),
                   &plan) == 0,
          "teacher DAG plans");

    CHECK(consolidate_dag(&plan, sources, 2, NULL, &chunk, NULL) == 0,
          "chunk distills");
    memset(&c, 0, sizeof c);
    CHECK(contract_from_dag(&plan, sources, 2, "pair_from_bsyms",
                            4096, &c) == 0 && c.exemplar_count == 4,
          "contract emitted from the same teacher plan");
    CHECK(btn_certify(&chunk, &c, NULL) == 0,
          "the chunk certifies against its teacher's contract");

    /* guard: an unconsumed source refuses */
    {
        DagSource three[3];
        three[0] = sources[0];
        three[1] = sources[1];
        three[2] = sources[0];
        CHECK(contract_from_dag(&plan, three, 3, "pair_from_bsyms",
                                4096, &c) == -1,
              "refuses an unconsumed source");
    }

    contract_free(&c);
    dag_free(&plan);
    btn_free(&chunk);
    registry_free(&reg);
    btn_free(&decoder2);
    btn_free(&combiner);
}
```

(test_certify.c must `#include "../include/consolidate.h"` for the chunk
test.) RAW-boundary refusal is already covered structurally by
`plan_table_build`; the cap test pins the same refusal path.

- [ ] **Step 5.2: Watch it fail.**
Run: `make test_certify; ./test_certify.exe`
Expected: emission CHECKs FAIL (stubs).

- [ ] **Step 5.3: Implement `contract_from_route` / `contract_from_dag`**
in `src/contract.c` — mirror the consolidate wrappers but feed a Contract
instead of a student (guards: route `length >= 1`, non-NULL steps; dag
`DAG_PRIMITIVE` root, `n_sources` in `[1, DAG_MAX_SLOTS]`, every source
consumed exactly once via `plan_dag_count_sources`; both: valid atom name):

```c
static int contract_from_table(Contract *out, const char *name,
                               const Port *in_ports, size_t n_in,
                               const Port *out_ports, size_t n_out,
                               PlanTeacherFn teacher, void *ctx,
                               BinaryTransformNetwork *const *members,
                               size_t n_members, size_t max_samples) {
    PlanTable table;
    size_t out_total = ports_total(out_ports, n_out);
    size_t i;

    if (out == NULL || !name_valid(name)) {
        return -1;
    }
    if (plan_table_build(in_ports, n_in, out_total, teacher, ctx,
                         members, n_members, max_samples, &table) != 0) {
        return -1;
    }
    if (table.kept == 0) {
        plan_table_free(&table);
        return -1;
    }

    memset(out, 0, sizeof *out);
    strcpy(out->name, name);
    for (i = 0; i < n_in; ++i) {
        out->input_ports[i] = in_ports[i];
    }
    out->input_port_count = n_in;
    for (i = 0; i < n_out; ++i) {
        out->output_ports[i] = out_ports[i];
    }
    out->output_port_count = n_out;
    out->inputs = table.inputs;     /* ownership transfers */
    out->outputs = table.targets;
    out->exemplar_count = table.kept;
    out->owns_data = 1;
    return 0;
}
```

`contract_from_route`: in_port = `steps[0]->input_ports[0]`; out_port =
`steps[length-1]->output_ports[0]`; members = the steps array; teacher =
`plan_route_teacher` with a strict RoutePlan copy.
`contract_from_dag`: in_ports = source types in index order (offsets as in
consolidate_dag); out_port = root's projected port; members collected via
`plan_dag_collect_members` (heap array, freed after); teacher =
`plan_dag_teacher` with a `PlanDagTeacherCtx`.

- [ ] **Step 5.4: Verify green.**
Run: `make test_certify; ./test_certify.exe` — all pass.
Run: `make test` — all suites pass.

---

### Task 6: nn_demo emits contracts beside every BTN weight file

**Files:**
- Modify: `src/main.c`
- Modify: `Makefile` (nn_demo rule)

- [ ] **Step 6.1: Link the new units into nn_demo.** Makefile rule becomes:

```make
nn_demo: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(APP) include/nn.h include/router.h include/plan_table.h include/contract.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(APP) $(LDFLAGS)
```

- [ ] **Step 6.2: Add the emission helper to `src/main.c`** (top of file,
after the includes; also `#include "../include/contract.h"`):

```c
/* The training table IS the spec: persist it as a first-class contract
   beside the weights. Borrowed tables -- nothing to free. */
static void emit_contract(const char *name,
                          const BinaryTransformNetwork *btn,
                          const double *inputs, const double *targets,
                          size_t exemplars, const char *path) {
    Contract c;

    if (contract_init_borrowed(&c, name, btn, inputs, targets,
                               exemplars) != 0 ||
        contract_save(&c, path) != 0) {
        fprintf(stderr, "WARN: could not emit contract %s.\n", path);
    }
}
```

- [ ] **Step 6.3: Call it after each BTN's `btn_save`** (the training arrays
are still in scope at each save site; exemplar counts are the training
sample counts already passed to `btn_train_dynamic`):

```c
emit_contract("increment", &increment, &inc_inputs[0][0],
              &inc_targets[0][0], 16, "increment_contract.txt");
emit_contract("combine", &combine_net, &combine_inputs[0][0],
              &combine_targets[0][0], 256, "combine_contract.txt");
emit_contract("split", &split_net, &combine_inputs[0][0],
              &combine_targets[0][0], 256, "split_contract.txt");
emit_contract("conditional_increment", &cond_inc, &cond_inc_inputs[0][0],
              &cond_inc_targets[0][0], 32, "cond_increment_contract.txt");
emit_contract("hex_value", &hex_values, &hex_value_inputs[0][0],
              &hex_value_targets[0][0], 16, "hex_value_contract.txt");
emit_contract("hex_char", &hex_chars, &hex_char_inputs[0][0],
              &hex_char_targets[0][0], 26, "hex_char_contract.txt");
emit_contract("word", &word_net, &word_inputs[0][0],
              &word_targets[0][0], WORD_COUNT, "word_contract.txt");
emit_contract("raw_word", &raw_word_net, &raw_word_inputs[0][0],
              &raw_word_targets[0][0], WORD_COUNT, "raw_word_contract.txt");
```

Each call goes immediately after the corresponding `btn_save(...)` (same
spot where stats sidecars are removed). Verify each array/count pair
against the `btn_train_dynamic` call right above it — the counts above
were read from src/main.c at planning time but MUST be re-checked against
the actual call sites during execution.

- [ ] **Step 6.4: Verify.**
Run: `make run`
Expected: demo output unchanged, plus 8 new `*_contract.txt` files exist.
Run: `make test` — green.
Spot-check: `Get-Content hex_value_contract.txt -TotalCount 7` shows the
CNET_CONTRACT header, ports with tags, `EXEMPLARS 16`.

---

### Task 7: `certify_demo` + `make certify`

**Files:**
- Create: `tests/certify_demo.c`
- Modify: `Makefile`

- [ ] **Step 7.1: Write `tests/certify_demo.c`.** Structure (full file;
follow chunk_demo's helper conventions — copy `P`, `bits_to_int` verbatim):

```c
/*
 * End-to-end certification demonstration.
 *
 * (1) Loads the frozen primitives AND their nn_demo-emitted contracts,
 *     certifies each against its own spec, and prints the verdict table.
 * (2) Imposter theater: increment claims hex_value's contract --
 *     registry_add_certified refuses (signature gate); a tampered
 *     hex_value contract is denied (behavior gate).
 * (3) Plans hex_digit -> incremented value with require_certified set:
 *     the returned chain is certified end-to-end; executes 16/16.
 * (4) Emits the contract of combine(hex_value, hex_value) from the proven
 *     plan, distills the chunk from the same plan, and certifies the
 *     chunk against the emitted contract -- the loop closes.
 *
 * Requires weight + contract files from ./nn_demo (make certify).
 */
```

Body outline with the assertions that gate the exit code:
- Load `hex_value`, `increment`, `combine`, `split` weights; load
  `hex_value_contract.txt`, `increment_contract.txt`,
  `combine_contract.txt`, `split_contract.txt` via `contract_load` (any
  failure = FAIL exit).
- Part 1: `btn_certify` each pair, print
  `  hex_value  vs hex_value_contract  : CERTIFIED (16/16)` style lines
  from the CertifyReport; any denial = failure.
- Part 2: `registry_add_certified(&reg, &incr, "imposter", &hexval_contract)`
  must return -1 (print "imposter refused (signature)"); then copy the
  hex_value contract struct, flip one output value
  (`tampered.outputs[5] = 1.0 - tampered.outputs[5];` on an owned COPY —
  build it by `contract_load`ing a second instance), certify -> must be
  -1 (print "tampered spec denied (behavior, report shows the miss)").
- Part 3: `registry_add_certified` hex_value + increment with their real
  contracts; `reg.require_certified = 1`; `route_plan(ONEHOT16 ->
  BINARY_MSB5)` must return the 2-hop chain; execute all 16 digits ->
  value+1 (reuse the loop from route_demo); print
  "certified end-to-end plan executed 16/16".
- Part 4: two ONEHOT16 sources, `dag_plan` (knob OFF — combine itself is
  registered plain here; set `reg.require_certified = 0` first) ->
  combine tree; `contract_from_dag(plan, sources, 2, "hex_pair_to_byte",
  4096, &emitted)`; `consolidate_dag(plan, sources, 2, NULL, &chunk,
  NULL)`; `btn_certify(&chunk, &emitted, &rep)` == 0; print
  "chunk certified against its teacher's emitted contract (256/256)".
- Free everything; print `CERTIFY PASS: ...` / `CERTIFY FAIL.` and exit
  0/1 accordingly.

- [ ] **Step 7.2: Makefile.**

```make
CERTIFY_DEMO := tests/certify_demo.c

# Certifies primitives against their contracts; imposters are refused.
certify_demo: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(CERTIFY_DEMO) include/nn.h include/router.h include/plan_table.h include/consolidate.h include/contract.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONSOLIDATE) $(CONTRACT) $(CERTIFY_DEMO) $(LDFLAGS)

# Regenerates frozen weights + contracts, then certifies and plans with
# the require_certified knob.
certify: nn_demo certify_demo
	./nn_demo
	./certify_demo
```

Add `certify` to `.PHONY`, `certify_demo` + both binary spellings to
`clean`.

- [ ] **Step 7.3: Verify.**
Run: `make certify`
Expected output shape: four CERTIFIED lines, "imposter refused", "tampered
spec denied", "certified end-to-end plan executed 16/16", "chunk certified
... (256/256)", `CERTIFY PASS`.

---

### Task 8: Docs + final verification

**Files:**
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-06-11-machine-checkable-contracts-design.md`

- [ ] **Step 8.1: README.** After the "Learned Reliability" section, add a
"## Machine-Checkable Contracts" section: what a contract file is, the two
certification gates, `registry_add_certified` + `require_certified`, both
emission sources, and `make certify` in the make-target table (one row).
Mention the chunk-certification loop in the Chunk Consolidation section
(one sentence).

- [ ] **Step 8.2: Spec status.** Flip the spec's Status line to
"Implemented 2026-06-11 (TDD; `make test` + `make certify` green)." and
record any deviations discovered during implementation.

- [ ] **Step 8.3: Full gate.**
Run: `make test` — six suites + test_certify all green.
Run: `make chunk` — CHUNK PASS (consolidation untouched in behavior).
Run: `make certify` — CERTIFY PASS.
Run: `make route; make dag` — still green (knob-off behavior unchanged).

---

## Self-Review (performed at planning time)

- Spec coverage: contract model/format (Task 2), certification gates +
  statelessness (Task 3), registry flag + knob + no-signature-changes
  (Task 4), both emission sources (Tasks 5, 6), plan_table refactor
  (Task 1), tests incl. punchline (Tasks 2-5), demo (Task 7), docs
  (Task 8). No gaps found.
- Placeholders: none — every code step shows the code or names an exact
  verbatim move/copy source.
- Type consistency: `PlanTable`/`plan_table_build` (1) used in 5;
  `Contract`/`CertifyReport` (2, 3) used in 4-7; `ports_total`/
  `name_valid` defined in 2.6, used in 3.3/5.3; `entry_usable` (4.4)
  router-local. Counts/arrays in Task 6 flagged for re-verification
  against main.c at execution time.
