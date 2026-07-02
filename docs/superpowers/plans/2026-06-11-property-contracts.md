# Property Contracts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Equational property contracts — named laws (`LHS chain = RHS chain` over a typed source domain) stored as data files, checked by strict replay against a registry; demo: `split ∘ combine = identity` both ways, plus a perturbed combine being caught.

**Architecture:** New `property` module (fixed-size Property struct, CNET_PROPERTY 1 format, three-stage `property_check`: resolve → static type gate → streaming replay via a stats-free chain runner). `plan_table.c`'s static domain enumeration is widened to an exported `PlanDomain` API. Spec: `docs/superpowers/specs/2026-06-11-property-contracts-design.md`.

**Tech Stack:** C11, GCC/MinGW, make. NOT a git repo — every "commit" gate is a verification gate (clean build + tests green). `src/nn.c`, `src/router.c`, `src/contract.c`, `src/consolidate.c` are NOT modified by this feature.

**House rules for the executor:**
- Build/run from repo root; flags come from the Makefile (`-mno-avx` is load-bearing). Zero warnings required.
- Test convention: `CHECK(cond, desc)` macro + `failures` counter + exit code, as in `tests/test_certify.c`.
- The property module must NOT include `contract.h` or `consolidate.h` (it is independent); it uses `nn.h`, `router.h`, `plan_table.h`.

---

### Task 1: Widen plan_table — export the domain enumeration

**Files:**
- Modify: `include/plan_table.h`
- Modify: `src/plan_table.c`

Behavior-preserving rename-to-export, pinned by the existing suites.

- [ ] **Step 1.1: Header.** In `include/plan_table.h`, after `plan_port_total`, add:

```c
/* The enumerated canonical domain of a port list, flattened to per-field
   units so the cartesian product is one loop. Later fields advance
   fastest. Build refuses RAW fields and size_t overflow. */
typedef struct {
    Port *field_port;     /* owning port, one entry per field */
    size_t *field_offset; /* start of the field within the input vector */
    size_t *cardinality;
    size_t n_fields;
    size_t combos;        /* product of cardinalities */
    size_t in_total;
} PlanDomain;

int  plan_domain_build(const Port *ports, size_t n_ports, PlanDomain *d);
void plan_domain_write(const PlanDomain *d, size_t sample, double *vec);
void plan_domain_free(PlanDomain *d);
```

- [ ] **Step 1.2: Source.** In `src/plan_table.c`: rename the static `Domain` struct to the exported `PlanDomain` (delete the local typedef; the header now provides it) and rename `domain_build` -> `plan_domain_build`, `domain_write` -> `plan_domain_write`, `domain_free` -> `plan_domain_free`, dropping `static` from all three. Update every internal reference (all inside `plan_table_build`). Bodies unchanged. `field_cardinality`/`field_write` stay static.

- [ ] **Step 1.3: Verify invisible.**
Run: `make test` — all seven suites green, zero warnings.
Run: `make chunk_demo; ./chunk_demo.exe` — CHUNK PASS, same counts (16/16, 256/256, 256/256).

---

### Task 2: Property struct, file format, save/load (TDD)

**Files:**
- Create: `include/property.h`
- Create: `src/property.c`
- Create: `tests/test_property.c`
- Modify: `Makefile`

- [ ] **Step 2.1: Create `include/property.h`** with exactly:

```c
#ifndef PROPERTY_H
#define PROPERTY_H

#include <stddef.h>

#include "nn.h"
#include "router.h"

/* An equational property: a NAMED LAW defined by data -- a typed source
   signature plus two chains of primitive names. It holds iff, for every
   canonical member of the enumerated source domain, strictly executing
   the LHS chain equals strictly executing the RHS chain (RHS empty =
   identity on the sources). Laws state what exemplar contracts cannot:
   relations BETWEEN primitives -- and they are the regression net for
   retraining (a broken combine violates split-after-combine = identity
   even if nobody wrote its exemplar table). */

#define PROPERTY_NAME_MAX 64   /* atoms over [A-Za-z0-9_], like tags */
#define PROPERTY_MAX_SOURCES BTN_MAX_INPUT_PORTS
#define PROPERTY_MAX_STEPS 8

typedef struct {
    char name[PROPERTY_NAME_MAX];
    Port sources[PROPERTY_MAX_SOURCES];
    size_t source_count;
    char lhs[PROPERTY_MAX_STEPS][PROPERTY_NAME_MAX];
    size_t lhs_len;                      /* >= 1 */
    char rhs[PROPERTY_MAX_STEPS][PROPERTY_NAME_MAX];
    size_t rhs_len;                      /* 0 = identity on the sources */
} Property;

/* Persist / restore ("CNET_PROPERTY 1"). Fixed-size struct: nothing to
   free. property_load validates magic+version, atom names, source count
   in [1, PROPERTY_MAX_SOURCES], chain lengths in bounds (LHS >= 1),
   known families, nonzero widths/counts, tags via port_set_tag. Returns
   0, or -1 on malformed input (*p untouched). */
int property_save(const Property *p, const char *path);
int property_load(Property *p, const char *path);

typedef struct {
    size_t inputs;     /* domain members checked */
    size_t held;       /* both sides clean and equal */
    size_t violated;   /* unclean handoff on either side, or mismatch */
} PropertyReport;

/* Check a law against a registry: (1) RESOLVE every chain name (first
   strcmp match wins); (2) STATIC TYPE GATE -- sources -> first step,
   step -> step (port counts equal, position-wise port_compatible), and
   LHS final vs RHS final equal in REPRESENTATION (family/width/count;
   tags may differ -- the law equates values, not labels); (3) REPLAY --
   enumerate the canonical source domain (RAW or over-max_samples
   refused) and run BOTH chains per input with strict validate-then-
   canonicalize at every handoff; an unclean value on either side, or a
   final canonical mismatch, counts that input as a violation. Never
   aborts early: the report shows how broken a broken law is. The chain
   runner sits on btn_forward, which records nothing -- checking is
   stateless. Returns 0 iff the replay ran with zero violations; -1
   otherwise (resolve/gate refusals leave report.inputs at 0). */
int property_check(const Property *p, const PrimitiveRegistry *reg,
                   size_t max_samples, PropertyReport *report);

#endif
```

- [ ] **Step 2.2: Stub `src/property.c`**: includes `../include/property.h`, `../include/plan_table.h`, `<stdio.h>`, `<stdlib.h>`, `<string.h>`; all three functions return -1 (property_check also zeroes the report if given).

- [ ] **Step 2.3: Write `tests/test_property.c`** — first slice (round-trip + malformed). Harness: copy `CHECK`/`failures` from tests/test_certify.c; local helper `PT(family, w, c, tag)` (same as test_certify's). Include `../include/nn.h`, `../include/router.h`, `../include/property.h`, `<stdio.h>`, `<string.h>`.

```c
static void fill_law(Property *p) {
    memset(p, 0, sizeof *p);
    strcpy(p->name, "unpack_inverts_pack");
    p->sources[0] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    p->sources[1] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    p->source_count = 2;
    strcpy(p->lhs[0], "pack");
    strcpy(p->lhs[1], "unpack");
    p->lhs_len = 2;
    p->rhs_len = 0;
}

static void test_roundtrip(void) {
    Property p, back;

    printf("property round-trip:\n");
    fill_law(&p);
    CHECK(property_save(&p, "tmp_property.txt") == 0, "saves");
    memset(&back, 0, sizeof back);
    CHECK(property_load(&back, "tmp_property.txt") == 0, "loads");
    CHECK(strcmp(back.name, "unpack_inverts_pack") == 0 &&
          back.source_count == 2 &&
          back.sources[0].family == PORT_BINARY_MSB &&
          strcmp(back.sources[1].tag, "bit") == 0 &&
          back.lhs_len == 2 && strcmp(back.lhs[0], "pack") == 0 &&
          strcmp(back.lhs[1], "unpack") == 0 && back.rhs_len == 0,
          "round-trip preserves name, sources, chains, identity RHS");

    {
        FILE *f = fopen("tmp_property_bad.txt", "w");
        if (f != NULL) { fputs("CNET_WRONG 1\n", f); fclose(f); }
        CHECK(property_load(&back, "tmp_property_bad.txt") == -1,
              "rejects a wrong magic");
    }
    {
        /* LHS 0 is not a law */
        FILE *f = fopen("tmp_property_bad.txt", "w");
        if (f != NULL) {
            fputs("CNET_PROPERTY 1\nx\nSOURCES 1\n"
                  "PORT binary_msb 1 1 bit\nLHS 0\nRHS 0\n", f);
            fclose(f);
        }
        CHECK(property_load(&back, "tmp_property_bad.txt") == -1,
              "rejects an empty LHS");
    }
    {
        FILE *f = fopen("tmp_property_bad.txt", "w");
        if (f != NULL) {
            fputs("CNET_PROPERTY 1\nbad name!\nSOURCES 1\n"
                  "PORT binary_msb 1 1 bit\nLHS 1\npack\nRHS 0\n", f);
            fclose(f);
        }
        CHECK(property_load(&back, "tmp_property_bad.txt") == -1,
              "rejects a non-atom name");
    }
    {
        FILE *f = fopen("tmp_property_bad.txt", "w");
        if (f != NULL) {
            fputs("CNET_PROPERTY 1\nx\nSOURCES 9\n", f);
            fclose(f);
        }
        CHECK(property_load(&back, "tmp_property_bad.txt") == -1,
              "rejects source count out of bounds");
    }
    {
        FILE *f = fopen("tmp_property_bad.txt", "w");
        if (f != NULL) {
            fputs("CNET_PROPERTY 1\nx\nSOURCES 1\n"
                  "PORT chrome 1 1 bit\nLHS 1\npack\nRHS 0\n", f);
            fclose(f);
        }
        CHECK(property_load(&back, "tmp_property_bad.txt") == -1,
              "rejects an unknown family");
    }
    CHECK(property_load(&back, "no_such_file.txt") == -1,
          "missing file returns -1");

    remove("tmp_property.txt");
    remove("tmp_property_bad.txt");
}
```

`main()` calls `test_roundtrip()`, prints "All property tests passed." / "%d property test(s) FAILED." and returns the failures-based exit code.

- [ ] **Step 2.4: Makefile.** Add (and wire `test_property` into the `test` target's deps + run lines, plus both spellings into `clean`):

```make
PROPERTY := src/property.c
PROPERTY_TEST := tests/test_property.c

test_property: $(SRC) $(ROUTER) $(PLAN_TABLE) $(PROPERTY) $(PROPERTY_TEST) include/nn.h include/router.h include/plan_table.h include/property.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(PROPERTY) $(PROPERTY_TEST) $(LDFLAGS)
```

- [ ] **Step 2.5: Watch it fail.**
Run: `make test_property; ./test_property.exe`
Expected: "saves"/"loads"/round-trip FAIL (stubs); malformed CHECKs pass vacuously.

- [ ] **Step 2.6: Implement** `property_save` / `property_load` in `src/property.c`:
- Static helpers (duplicated from contract.c by design — the modules are independent and nn.c is untouchable; ~20 lines): `family_token`, `family_parse`, `name_valid` (nonempty, < PROPERTY_NAME_MAX, `[A-Za-z0-9_]`).
- `property_save`: refuse NULL/invalid name/`source_count` out of `[1, PROPERTY_MAX_SOURCES]`/`lhs_len` out of `[1, PROPERTY_MAX_STEPS]`/`rhs_len > PROPERTY_MAX_STEPS`/invalid chain atoms/unknown family. Write exactly: `CNET_PROPERTY 1\n`, `<name>\n`, `SOURCES n\n`, one `PORT <family> <width> <count> <tag-or-->\n` per source, `LHS k\n`, k name lines, `RHS m\n`, m name lines. Check every write; fail -> fclose + -1 (final fclose checked, returning -1 directly as in contract_save).
- `property_load`: parse into a zeroed LOCAL Property with bounded `%63s`/`%31s` reads; validate everything per the header doc (magic+version, atoms, bounds, families via family_parse, widths/counts nonzero, tags via `port_set_tag` with `-` = untagged); commit `*p = local` only at the end; any failure -> fclose + -1, *p untouched.

- [ ] **Step 2.7: Verify green.**
Run: `make test_property; ./test_property.exe` — all pass.
Run: `make test` — all eight suites green.

---

### Task 3: property_check — resolve, type gate, replay (TDD)

**Files:**
- Modify: `src/property.c`
- Modify: `tests/test_property.c`

- [ ] **Step 3.1: Add the failing tests.** Helpers first — tiny inverse pair. `make_pack` is the combiner shape (identity on two concatenated bits); `make_unpack` is its inverse with two output ports:

```c
/* Two MSB-first bits of i (0..3). */
static void msb2(int i, double *out) {
    out[0] = (double)((i >> 1) & 1);
    out[1] = (double)(i & 1);
}

/* pack: [BINARY_MSB1 "bit", BINARY_MSB1 "bit"] -> BINARY_MSB2 "pair". */
static int make_pack(BinaryTransformNetwork *b) {
    double inputs[4][2];
    double targets[4][2];
    Port in_ports[2];
    int i;
    if (btn_init(b, 2, 2, 1, 16, 0.8, 19u) != 0) return -1;
    in_ports[0] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    in_ports[1] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    if (btn_set_input_ports(b, in_ports, 2,
                            PT(PORT_BINARY_MSB, 2, 1, "pair")) != 0) return -1;
    for (i = 0; i < 4; ++i) {
        msb2(i, inputs[i]);
        msb2(i, targets[i]);
    }
    return btn_train_dynamic(b, &inputs[0][0], &targets[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* unpack: BINARY_MSB2 "pair" -> [BINARY_MSB1 "bit", BINARY_MSB1 "bit"]. */
static int make_unpack(BinaryTransformNetwork *b) {
    double inputs[4][2];
    double targets[4][2];
    Port in_port = {0};
    Port out_ports[2];
    int i;
    if (btn_init(b, 2, 2, 1, 16, 0.8, 29u) != 0) return -1;
    in_port = PT(PORT_BINARY_MSB, 2, 1, "pair");
    out_ports[0] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    out_ports[1] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    if (btn_set_io_ports(b, &in_port, 1, out_ports, 2) != 0) return -1;
    for (i = 0; i < 4; ++i) {
        msb2(i, inputs[i]);
        msb2(i, targets[i]);
    }
    return btn_train_dynamic(b, &inputs[0][0], &targets[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}
```

Then the suites (both called from main after test_roundtrip):

```c
static void test_laws_hold(void) {
    BinaryTransformNetwork pack = {0};
    BinaryTransformNetwork unpack = {0};
    PrimitiveRegistry reg;
    Property p;
    PropertyReport rep;

    printf("property_check (laws hold):\n");
    CHECK(make_pack(&pack) == 0 && make_unpack(&unpack) == 0,
          "inverse pair trains");
    registry_init(&reg);
    registry_add(&reg, &pack, "pack");
    registry_add(&reg, &unpack, "unpack");

    pack.output_successes = 7;
    pack.output_failures = 3;

    fill_law(&p);  /* unpack(pack(a,b)) = identity */
    memset(&rep, 0, sizeof rep);
    CHECK(property_check(&p, &reg, 4096, &rep) == 0,
          "unpack inverts pack: the law holds");
    CHECK(rep.inputs == 4 && rep.held == 4 && rep.violated == 0,
          "report counts the full domain");
    CHECK(pack.output_successes == 7 && pack.output_failures == 3,
          "checking records no reliability evidence");

    /* the other direction: pack(unpack(p)) = identity on pairs */
    memset(&p, 0, sizeof p);
    strcpy(p.name, "pack_inverts_unpack");
    p.sources[0] = PT(PORT_BINARY_MSB, 2, 1, "pair");
    p.source_count = 1;
    strcpy(p.lhs[0], "unpack");
    strcpy(p.lhs[1], "pack");
    p.lhs_len = 2;
    p.rhs_len = 0;
    memset(&rep, 0, sizeof rep);
    CHECK(property_check(&p, &reg, 4096, &rep) == 0 &&
          rep.inputs == 4 && rep.held == 4,
          "pack inverts unpack: the law holds");

    registry_free(&reg);
    btn_free(&pack);
    btn_free(&unpack);
}

static void test_violation_and_gates(void) {
    BinaryTransformNetwork pack = {0};
    BinaryTransformNetwork unpack = {0};
    PrimitiveRegistry reg;
    Property p;
    PropertyReport rep;

    printf("property_check (violations + gates):\n");
    CHECK(make_pack(&pack) == 0 && make_unpack(&unpack) == 0,
          "inverse pair trains");
    registry_init(&reg);
    registry_add(&reg, &pack, "pack");
    registry_add(&reg, &unpack, "unpack");

    /* a broken retrain cannot hide from the algebra */
    unpack.hidden_output_weights[0] += 4.0;
    fill_law(&p);
    memset(&rep, 0, sizeof rep);
    CHECK(property_check(&p, &reg, 4096, &rep) == -1,
          "a perturbed implementation violates the law");
    CHECK(rep.inputs == 4 && rep.violated >= 1 &&
          rep.held + rep.violated == 4,
          "report shows how broken it is");
    unpack.hidden_output_weights[0] -= 4.0;

    /* static gates: each refuses with no inputs checked */
    fill_law(&p);
    strcpy(p.lhs[0], "nosuch");
    memset(&rep, 0, sizeof rep);
    CHECK(property_check(&p, &reg, 4096, &rep) == -1 && rep.inputs == 0,
          "unresolvable name refuses before any execution");

    fill_law(&p);
    p.source_count = 1;  /* one bit source vs pack's two input ports */
    CHECK(property_check(&p, &reg, 4096, NULL) == -1,
          "source/step port-count mismatch refuses");

    fill_law(&p);
    p.lhs_len = 1;  /* pack alone: final [pair] vs identity [bit,bit] */
    CHECK(property_check(&p, &reg, 4096, NULL) == -1,
          "LHS/RHS representation mismatch refuses");

    fill_law(&p);
    p.sources[0] = PT(PORT_RAW, 1, 1, NULL);
    CHECK(property_check(&p, &reg, 4096, NULL) == -1,
          "a RAW source is not enumerable");

    fill_law(&p);
    CHECK(property_check(&p, &reg, 3, NULL) == -1,
          "refuses when the domain exceeds max_samples");

    fill_law(&p);
    p.lhs_len = 0;
    CHECK(property_check(&p, &reg, 4096, NULL) == -1,
          "an empty LHS is not a law");

    registry_free(&reg);
    btn_free(&pack);
    btn_free(&unpack);
}
```

- [ ] **Step 3.2: Watch it fail.**
Run: `make test_property; ./test_property.exe`
Expected red: "the law holds" CHECKs and both report-count CHECKs FAIL (stub returns -1); gate CHECKs pass vacuously. Record output.

- [ ] **Step 3.3: Implement `property_check`** (replace the stub; the statics go above it):

```c
static const BinaryTransformNetwork *resolve(const PrimitiveRegistry *reg,
                                             const char *name) {
    size_t i;

    for (i = 0; i < reg->count; ++i) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            return reg->entries[i].btn;
        }
    }
    return NULL;
}

/* Position-wise compatibility for a handoff: same port count, each
   produced port port_compatible with the consuming port. */
static int seq_compatible(const Port *prod, size_t np,
                          const Port *cons, size_t nc) {
    size_t i;

    if (np != nc) {
        return 0;
    }
    for (i = 0; i < np; ++i) {
        if (!port_compatible(prod[i], cons[i])) {
            return 0;
        }
    }
    return 1;
}

/* The equation compares VALUES: representation must agree exactly, tags
   are free to differ. */
static int seq_same_representation(const Port *a, size_t na,
                                   const Port *b, size_t nb) {
    size_t i;

    if (na != nb) {
        return 0;
    }
    for (i = 0; i < na; ++i) {
        if (a[i].family != b[i].family ||
            a[i].field_width != b[i].field_width ||
            a[i].field_count != b[i].field_count) {
            return 0;
        }
    }
    return 1;
}

/* Strictly validate and canonicalize vec against a port sequence,
   writing the canonical form to clean. Returns 0, or -1 on any
   out-of-domain slice. */
static int seq_canonicalize(const Port *ports, size_t n,
                            const double *vec, double *clean) {
    size_t off = 0;
    size_t i;

    for (i = 0; i < n; ++i) {
        if (!port_validate(ports[i], vec + off) ||
            port_canonicalize(ports[i], vec + off, clean + off) != 0) {
            return -1;
        }
        off += plan_port_total(ports[i]);
    }
    return 0;
}

/* Run a chain on a canonical input vector, strict at every handoff.
   Returns 0 with the canonical result in out, -1 on any unclean value.
   n_steps 0 = identity (copies in to out). */
static int chain_run(const BinaryTransformNetwork *const *steps,
                     size_t n_steps, const double *in, size_t in_total,
                     double *out, double *buf_a, double *buf_b) {
    const double *cur = in;
    size_t cur_len = in_total;
    size_t s;

    if (n_steps == 0) {
        memcpy(out, in, in_total * sizeof *out);
        return 0;
    }
    for (s = 0; s < n_steps; ++s) {
        BinaryTransformNetwork *p = (BinaryTransformNetwork *)steps[s];
        double *stage = (s % 2 == 0) ? buf_a : buf_b;
        double *dest;
        const double *raw;

        if (seq_canonicalize(p->input_ports, p->input_port_count,
                             cur, stage) != 0) {
            return -1;
        }
        raw = btn_forward(p, stage);
        dest = (s + 1 == n_steps) ? out : stage;
        if (raw == NULL ||
            seq_canonicalize(p->output_ports, p->output_port_count,
                             raw, dest) != 0) {
            return -1;
        }
        cur = dest;
        cur_len = p->output_count;
    }
    (void)cur_len;
    return 0;
}
```

CAREFUL with the buffers in chain_run: `stage` holds the canonicalized
input for the forward pass, and the canonical OUTPUT may be written over
the same stage buffer only AFTER btn_forward has consumed it — that is
safe because btn_forward copies nothing lazily (it reads inputs fully
while computing hidden activations, then writes to its own last_output;
`raw` points to btn-internal memory, not to stage). The implementer must
verify that reasoning against src/nn.c's btn_forward before relying on
it; if uncomfortable, use three buffers instead — correctness first.

Then the main function:

```c
int property_check(const Property *p, const PrimitiveRegistry *reg,
                   size_t max_samples, PropertyReport *report) {
    const BinaryTransformNetwork *lhs[PROPERTY_MAX_STEPS];
    const BinaryTransformNetwork *rhs[PROPERTY_MAX_STEPS];
    PlanDomain dom;
    double *in_vec = NULL;
    double *lhs_out = NULL;
    double *rhs_out = NULL;
    double *buf_a = NULL;
    double *buf_b = NULL;
    size_t max_width;
    size_t out_total;
    const Port *lhs_final;
    size_t lhs_final_n;
    const Port *rhs_final;
    size_t rhs_final_n;
    size_t s, i;
    int rc = -1;

    if (report != NULL) {
        memset(report, 0, sizeof *report);
    }
    if (p == NULL || reg == NULL ||
        p->source_count == 0 || p->source_count > PROPERTY_MAX_SOURCES ||
        p->lhs_len == 0 || p->lhs_len > PROPERTY_MAX_STEPS ||
        p->rhs_len > PROPERTY_MAX_STEPS) {
        return -1;
    }

    /* 1. RESOLVE */
    for (s = 0; s < p->lhs_len; ++s) {
        if ((lhs[s] = resolve(reg, p->lhs[s])) == NULL) {
            return -1;
        }
    }
    for (s = 0; s < p->rhs_len; ++s) {
        if ((rhs[s] = resolve(reg, p->rhs[s])) == NULL) {
            return -1;
        }
    }

    /* 2. STATIC TYPE GATE */
    {
        const Port *prev = p->sources;
        size_t prev_n = p->source_count;

        for (s = 0; s < p->lhs_len; ++s) {
            if (!seq_compatible(prev, prev_n, lhs[s]->input_ports,
                                lhs[s]->input_port_count)) {
                return -1;
            }
            prev = lhs[s]->output_ports;
            prev_n = lhs[s]->output_port_count;
        }
        lhs_final = prev;
        lhs_final_n = prev_n;

        prev = p->sources;
        prev_n = p->source_count;
        for (s = 0; s < p->rhs_len; ++s) {
            if (!seq_compatible(prev, prev_n, rhs[s]->input_ports,
                                rhs[s]->input_port_count)) {
                return -1;
            }
            prev = rhs[s]->output_ports;
            prev_n = rhs[s]->output_port_count;
        }
        rhs_final = prev;
        rhs_final_n = prev_n;

        if (!seq_same_representation(lhs_final, lhs_final_n,
                                     rhs_final, rhs_final_n)) {
            return -1;
        }
    }

    /* 3. REPLAY over the enumerated domain */
    if (plan_domain_build(p->sources, p->source_count, &dom) != 0) {
        return -1;
    }
    if (dom.combos > max_samples) {
        plan_domain_free(&dom);
        return -1;
    }

    out_total = 0;
    for (i = 0; i < lhs_final_n; ++i) {
        out_total += plan_port_total(lhs_final[i]);
    }
    max_width = dom.in_total > out_total ? dom.in_total : out_total;
    for (s = 0; s < p->lhs_len; ++s) {
        if (lhs[s]->input_count > max_width) max_width = lhs[s]->input_count;
        if (lhs[s]->output_count > max_width) max_width = lhs[s]->output_count;
    }
    for (s = 0; s < p->rhs_len; ++s) {
        if (rhs[s]->input_count > max_width) max_width = rhs[s]->input_count;
        if (rhs[s]->output_count > max_width) max_width = rhs[s]->output_count;
    }

    in_vec = malloc(dom.in_total * sizeof *in_vec);
    lhs_out = malloc(out_total * sizeof *lhs_out);
    rhs_out = malloc(out_total * sizeof *rhs_out);
    buf_a = malloc(max_width * sizeof *buf_a);
    buf_b = malloc(max_width * sizeof *buf_b);
    if (in_vec == NULL || lhs_out == NULL || rhs_out == NULL ||
        buf_a == NULL || buf_b == NULL) {
        goto done;
    }

    {
        size_t violated = 0;

        for (s = 0; s < dom.combos; ++s) {
            int clean;

            plan_domain_write(&dom, s, in_vec);
            clean = chain_run(lhs, p->lhs_len, in_vec, dom.in_total,
                              lhs_out, buf_a, buf_b) == 0 &&
                    chain_run(rhs, p->rhs_len, in_vec, dom.in_total,
                              rhs_out, buf_a, buf_b) == 0;
            if (clean) {
                for (i = 0; i < out_total; ++i) {
                    if (lhs_out[i] != rhs_out[i]) {
                        clean = 0;
                        break;
                    }
                }
            }
            if (report != NULL) {
                ++report->inputs;
                if (clean) {
                    ++report->held;
                } else {
                    ++report->violated;
                }
            }
            if (!clean) {
                ++violated;
            }
        }
        rc = violated == 0 ? 0 : -1;
    }

done:
    free(buf_b);
    free(buf_a);
    free(rhs_out);
    free(lhs_out);
    free(in_vec);
    plan_domain_free(&dom);
    return rc;
}
```

(If `report == NULL` the loop still runs fully — the local `violated`
counter decides rc. The identity RHS works because rhs_len 0 makes
chain_run copy the canonical input, and the static gate has already
proven the source representation equals the LHS final representation.)

- [ ] **Step 3.4: Verify green.**
Run: `make test_property; ./test_property.exe` — all pass (report the count).
Run: `make test` — all eight suites green, zero warnings.

---

### Task 4: property_demo + make property

**Files:**
- Create: `tests/property_demo.c`
- Modify: `Makefile`

- [ ] **Step 4.1: Write `tests/property_demo.c`.** Header comment:

```c
/*
 * End-to-end property (equational law) demonstration.
 *
 * (1) Authors the two round-trip laws in code, persists them as
 *     property files, reloads them, and checks both against the real
 *     frozen primitives:
 *         split(combine(hi, lo)) = identity   (256 nibble pairs)
 *         combine(split(b))      = identity   (256 bytes)
 * (2) The regression story: a second combine instance gets one weight
 *     perturbed in memory and is registered in a fresh registry; the
 *     first law now reports real violations -- a broken retrain cannot
 *     hide from the algebra, even with no exemplar contract in sight.
 *
 * Requires weight files from ./nn_demo. Run from the repo root
 * (make property).
 */
```

Structure (house demo voice; helper `PT` as in the tests; FAILs to
stderr; exit code from an `rc` accumulator):

- Load `combine_weights.txt` -> combine, `split_weights.txt` -> split
  (failure: "run ./nn_demo first", exit 1). registry_init + register
  as "combine" / "split".
- Author law 1 in code: name `split_inverts_combine`, sources
  [PT(PORT_BINARY_MSB,4,1,"nibble_value") x2], lhs ["combine","split"],
  rhs_len 0. `property_save(&law1, "split_inverts_combine_property.txt")`.
- Author law 2: name `combine_inverts_split`, sources
  [PT(PORT_BINARY_MSB,8,1,"byte_value")], lhs ["split","combine"],
  rhs_len 0. Save as `combine_inverts_split_property.txt`.
- Reload BOTH via property_load (round-trip on real files; assert 0).
- Check both with max_samples 4096 and a report; assert rc 0 and
  inputs == 256 for each; print e.g.
  `  split_inverts_combine : HOLDS (256/256)`.
- Regression story: btn_load a SECOND combine instance -> combine2;
  `combine2.hidden_output_weights[0] += 4.0;` register combine2 +
  split in a FRESH registry under the same names; re-check law 1;
  assert rc -1 and violated >= 1; print
  `  perturbed combine: VIOLATED (held X/256) -- a broken retrain
  cannot hide from the algebra`.
- Free both registries and all three btns; banner:
  `PROPERTY PASS: the algebra holds, and a broken implementation is
  caught by a law, not by luck.` / `PROPERTY FAIL.`; exit accordingly.

- [ ] **Step 4.2: Makefile.** Add (plus `property` to `.PHONY`, both demo
spellings to `clean`):

```make
PROPERTY_DEMO := tests/property_demo.c

# Checks equational laws over real primitives; catches a broken retrain.
property_demo: $(SRC) $(ROUTER) $(PLAN_TABLE) $(PROPERTY) $(PROPERTY_DEMO) include/nn.h include/router.h include/plan_table.h include/property.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(PROPERTY) $(PROPERTY_DEMO) $(LDFLAGS)

# Regenerates frozen weights, then checks the round-trip laws.
property: nn_demo property_demo
	./nn_demo
	./property_demo
```

- [ ] **Step 4.3: Verify.**
Run: `make property_demo; ./property_demo.exe` (weights exist — fast).
Expected: both HOLDS lines (256/256), the VIOLATED line with real counts,
PROPERTY PASS, exit 0. Run it twice (rerun safety).
Then: `make property` (full retrain gate) — PROPERTY PASS.
Run: `make test` — all eight suites green.

---

### Task 5: Docs + final verification

**Files:**
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-06-11-property-contracts-design.md`

- [ ] **Step 5.1: README.** Make ONLY these additions (the file has
user-edited regions — change nothing else):

(a) Immediately after the "## Machine-Checkable Contracts" section (it
ends with the paragraph about the two emission sources, right before
"## Chunk Consolidation"), insert:

```markdown
## Property Contracts (Laws)

Exemplar contracts certify one transform; property contracts state
relations BETWEEN transforms. A `<name>_property.txt` file
(`CNET_PROPERTY 1`) holds an equation: a typed source signature and two
chains of primitive names — left side, right side (an empty right side
means identity). `property_check` resolves the names against a registry,
type-checks every seam statically, then enumerates the canonical source
domain and strictly executes both chains on every input. Any unclean
handoff or canonical mismatch is a violation, and the report counts how
broken a broken law is:

```text
split_inverts_combine : HOLDS (256/256)
perturbed combine     : VIOLATED — a broken retrain cannot hide
                        from the algebra
```

Laws are the regression net for retraining: a wrong `combine` violates
`split(combine(hi, lo)) = identity` even if its own exemplar contract
was never written.
```

(b) Make Targets table — add after the `make certify` row:

```markdown
| `make property` | retrain, then check the round-trip laws and catch a perturbed implementation |
```

(c) File Formats — add after the contract-files bullet:

```markdown
- **Property files** (`CNET_PROPERTY 1`): an equational law — typed
  sources plus two chains of primitive names (empty right side =
  identity), checked by strict replay over the enumerated domain.
```

(d) In the `make test` row, the suite list gains "properties" (it
currently ends with "certification").

- [ ] **Step 5.2: Spec status.** In
`docs/superpowers/specs/2026-06-11-property-contracts-design.md`, flip
Status to: "Implemented 2026-06-11 (TDD; `make test` + `make property`
green)." plus any deviations discovered during implementation.

- [ ] **Step 5.3: Full gate** (run sequentially; paste verdict lines):
1. `make test` — eight suites green.
2. `make property` — PROPERTY PASS.
3. `make certify` — CERTIFY PASS.
4. `make chunk` — CHUNK PASS.

---

## Self-Review (performed at planning time)

- Spec coverage: model+format (Task 2), three-stage check incl. identity
  and statelessness (Task 3), plan_table widening (Task 1), tests incl.
  violation + every static gate (Tasks 2-3), demo incl. regression story
  (Task 4), docs (Task 5). The spec's YAGNI list maps to no task —
  correct.
- Placeholders: none; all code shown, file-format strings exact.
- Type consistency: `PlanDomain`/`plan_domain_*` (Task 1) used in Task 3;
  `Property`/`PropertyReport`/`PROPERTY_*` (Task 2) used in Tasks 3-4;
  `fill_law` defined in Task 2's test file, reused in Task 3's tests;
  `make_pack` seed 19u / `make_unpack` seed 29u consistent.
- Buffer-aliasing risk in chain_run is explicitly flagged for implementer
  verification against btn_forward (three-buffer fallback allowed).
