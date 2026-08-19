# QAT Modern Block Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `cce_transformer_qat` able to express a modern Llama/Qwen-shaped block (RMSNorm, RoPE, GQA, SwiGLU, no bias) while zero-init config still produces today's exact GPT-2-shaped model.

**Architecture:** Five ops become composable units selected by config fields on `cce_transformer_qat_config`. Zero-init = legacy, so the existing Supra path and its gate stay green. Two prerequisites land first: the safetensors-bound loader moves to its own translation unit (making the hermetic gate linkable without `$(CCE)`), and the gradcheck's hand-built group array becomes a structural registry (closing a hole where unregistered parameters are silently ungradchecked).

**Tech Stack:** C11 (`-Wall -Wextra -pedantic -mno-avx`), no new dependencies. Hand-rolled forward/backward with directional gradcheck.

**Spec:** [docs/superpowers/specs/2026-08-16-qat-modern-block-design.md](../specs/2026-08-16-qat-modern-block-design.md)

## Global Constraints

- **C11**, `-Wall -Wextra -pedantic`. Silence unused params with `(void)param;`.
- **`-mno-avx` is mandatory** on this toolchain (MinGW gcc 15.2 AVX struct-copy bug).
- **Zero-init config MUST equal today's model.** Every new enum's legacy value is `0`; every new flag's legacy value is `0`. This is the compatibility contract and Task 3's gate enforces it.
- **`P` layout is frozen:** `{float *w,*g,*m,*v; int in,out;}`, `[in][out]` row-major, bias as `in == 1`. Per-OUTPUT column absmean scale orientation matches `cce_block_quantize_ternary`. The ternary export pipeline depends on all of it.
- **Config validation refuses, never clamps** — return `NULL` from `create` on any invalid combination (spec §4).
- **Every new parameter group must be registered** via the Task 2 registry. An unregistered group is silently ungradchecked.
- **Gradcheck tolerance is `rel < 5e-3`**, matching `tests/test_transformer_qat.c`.
- Test convention: `CHECK(cond, msg)` incrementing `checks`/`fails`; final line `ALL <NAME> TESTS PASSED` grepped by `tests/verify_logs.sh`.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/cce/cce_transformer_qat.c` (modify) | Trainer core: params, forward, backward, Adam, gradcheck. Loses all safetensors symbols. |
| `src/cce/cce_transformer_qat_load.c` (create) | `cce_transformer_qat_load_decomposed` + `copy_into_P` + `supra_last_block`. The only TU touching `$(CCE)`. |
| `include/cce/cce_transformer_qat.h` (modify) | Config enums/fields, group-registry accessors. |
| `tests/test_qat_block.c` (create) | Hermetic gate: registration audit, progressive gradchecks, legacy byte-identity, RoPE properties, GQA accumulation. |
| `Makefile` (modify) | `QAT_CORE_SRC`, `qat_block` target (no `$(CCE)`), add to `verify`. |
| `tests/verify_logs.sh` (modify) | Expectation line. |

---

## Task 1: Split the loader, add a hermetic gate target

Unblocks everything: without this, no gate in this plan can run on a box lacking curl/mmap/fsync.

**Files:**
- Create: `src/cce/cce_transformer_qat_load.c`
- Modify: `src/cce/cce_transformer_qat.c` (remove lines 342–439 region and the safetensors include)
- Create: `tests/test_qat_block.c`
- Modify: `Makefile`, `tests/verify_logs.sh`

**Interfaces:**
- Produces: `QAT_CORE_SRC := src/cce/cce_transformer_qat.c` and a `qat_block` make target linking only that plus the test. All later tasks use this target.

- [ ] **Step 1: Write the failing test**

Create `tests/test_qat_block.c`:

```c
/* QAT modern block gate (hermetic — no model files, no $(CCE) link).
   Spec: docs/superpowers/specs/2026-08-16-qat-modern-block-design.md */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_transformer_qat.h"

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } \
                              else printf("  ok   %s\n", msg); } while (0)

#define V_   24
#define T_   6

static void make_seq(int* seq, int* tgt) {
    unsigned long long r = 0xC0FFEEULL;
    for (int k = 0; k < T_; k++) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        seq[k] = (int)((r >> 33) % V_);
    }
    *tgt = (3 * seq[T_ - 1] + 7) % V_;
}

/* Legacy config: zero-init plus dims. MUST stay the GPT-2 shape. */
static void cfg_legacy(cce_transformer_qat_config* c) {
    memset(c, 0, sizeof *c);
    c->n_layer = 2; c->n_embd = 8; c->n_head = 2;
    c->mlp_hidden = 16; c->vocab = V_; c->block_size = 16;
    c->seed = 42;
}

int main(void) {
    int seq[T_], tgt;
    printf("=== QAT modern block gate ===\n");
    make_seq(seq, &tgt);

    printf("[1] hermetic core links and the legacy gradcheck still passes\n");
    {
        cce_transformer_qat_config c;
        cce_transformer_qat* t;
        cfg_legacy(&c);
        t = cce_transformer_qat_create(&c);
        CHECK(t != NULL, "legacy trainer creates");
        if (t) {
            double rel = cce_transformer_qat_gradcheck(t, seq, T_, tgt, 6);
            printf("  info legacy gradcheck rel err = %.3e\n", rel);
            CHECK(rel < 5e-3, "legacy backward matches central differences");
            cce_transformer_qat_free(t);
        }
    }

    printf("checks=%d fails=%d\n", checks, fails);
    if (fails) return 1;
    printf("ALL QAT BLOCK TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Add the Makefile target and run it to verify it fails**

Near the other CCE source variables, add:

```make
QAT_CORE_SRC := src/cce/cce_transformer_qat.c
QAT_LOAD_SRC := src/cce/cce_transformer_qat_load.c
```

Add the target next to `transformer_qat` (Makefile ~line 2324):

```make
# Hermetic QAT block gate: links the trainer CORE ONLY — no $(CCE), so no
# curl/mmap/fsync/POSIX-mkdir dependency. Builds on every box, which is what
# makes the gradcheck gates in this arc actually runnable.
qat_block: $(QAT_CORE_SRC) tests/test_qat_block.c include/cce/cce_transformer_qat.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(QAT_CORE_SRC) tests/test_qat_block.c -lm
	./$(BIN_DIR)/qat_block > logs/qat_block.log 2>&1
```

Run: `make qat_block`
Expected: FAIL at link — undefined references to `cce_forest_get_resident`, `cce_supra_head_fp`, because the core still contains `load_decomposed`.

- [ ] **Step 3: Move the loader into its own translation unit**

Create `src/cce/cce_transformer_qat_load.c`. Move **verbatim** from `cce_transformer_qat.c`: `copy_into_P`, `supra_last_block`, and `cce_transformer_qat_load_decomposed` (currently lines ~342–439). The new file's header:

```c
/* Real-checkpoint loading for the QAT trainer — the ONLY translation unit
   that binds it to $(CCE). Split out of cce_transformer_qat.c so the trainer
   core, and therefore the hermetic gradcheck gate, links without safetensors
   / gguf / kv_page and builds on any platform.
   Spec: docs/superpowers/specs/2026-08-16-qat-modern-block-design.md section 3 */

#include "../../include/cce/cce_transformer_qat.h"
#include "../../include/cce/cce_transformer_qat_internal.h"
#include "../../include/cce/cce_safetensors.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
```

The moved code needs the `P` type and the `struct cce_transformer_qat` layout, which are currently private to the .c file. Create `include/cce/cce_transformer_qat_internal.h` holding **only** the `P` typedef, `p_alloc`/`p_free` declarations, and the `struct cce_transformer_qat` definition, moved out of `cce_transformer_qat.c`. Both .c files include it. Mark it clearly:

```c
/* INTERNAL to the QAT trainer — not a public API. Exists so the loader TU
   (cce_transformer_qat_load.c) can see the parameter layout without the
   trainer core having to depend on $(CCE). */
```

Change `p_alloc`/`p_free` from `static` to non-static and declare them in the internal header, since the loader does not use them but `copy_into_P` may.

Then in `cce_transformer_qat.c`: delete the moved functions and **remove** `#include "../../include/cce/cce_safetensors.h"`.

- [ ] **Step 4: Add the loader to the existing full target**

`transformer_qat` and any other target listing `$(CCE)` must also compile the new loader TU. `$(CCE)` is a source list — add `src/cce/cce_transformer_qat_load.c` to it so every existing consumer keeps `load_decomposed`.

Find the `CCE :=` definition and append the loader source.

- [ ] **Step 5: Run the hermetic gate to verify it passes**

Run: `make qat_block && cat logs/qat_block.log`
Expected: `ALL QAT BLOCK TESTS PASSED`, with the legacy gradcheck rel err below 5e-3.

- [ ] **Step 6: Wire into verify**

Add `qat_block` to the `verify:` prerequisite list in `Makefile` (after `attribution`), and add to `tests/verify_logs.sh`:

```
qat_block.log|||ALL QAT BLOCK TESTS PASSED
```

- [ ] **Step 7: Commit**

```bash
git add src/cce/cce_transformer_qat.c src/cce/cce_transformer_qat_load.c include/cce/cce_transformer_qat_internal.h tests/test_qat_block.c Makefile tests/verify_logs.sh
git -c user.email="7261266+Mokason@users.noreply.github.com" commit -m "refactor(qat): split the safetensors-bound loader into its own TU

The trainer core now links without \$(CCE), so the hermetic gradcheck gate
builds on boxes lacking curl/mmap/fsync/POSIX-mkdir — where make
transformer_qat cannot build at all. New qat_block target, in verify.

Public API unchanged: same header, same symbol, different translation unit.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 2: Structural group registry (closes the silent gradcheck hole)

**Files:** Modify `src/cce/cce_transformer_qat.c`, `include/cce/cce_transformer_qat_internal.h`, `tests/test_qat_block.c`

**Interfaces:**
- Produces: `void qat_group_reset(cce_transformer_qat* t);`, `int qat_group_add(cce_transformer_qat* t, P* p, const char* name);`, `int cce_transformer_qat_group_count(const cce_transformer_qat* t);`, `int cce_transformer_qat_param_count(const cce_transformer_qat* t);`. Gradcheck and every later task use `qat_group_add`.

### Why

`cce_transformer_qat_gradcheck` currently walks a hand-built `P* groups[128]` with `if (ng + 12 > 124) break;`. Any group not appended is silently ungradchecked, and SwiGLU's 13th per-layer matrix breaks the bound. Registration must become a side effect of allocation, not a duplicated list.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_qat_block.c` before the `checks=%d` line:

```c
    printf("[2] every allocated parameter group is gradcheck-reachable\n");
    {
        cce_transformer_qat_config c;
        cce_transformer_qat* t;
        cfg_legacy(&c);
        t = cce_transformer_qat_create(&c);
        CHECK(t != NULL, "trainer creates");
        if (t) {
            int ng = cce_transformer_qat_group_count(t);
            int np = cce_transformer_qat_param_count(t);
            /* legacy: tok_emb + pos_emb + 12/layer + lnf_w/b + head_w/b */
            int expect = 2 + 12 * c.n_layer + 4;
            printf("  info groups=%d params=%d expected_groups=%d\n",
                   ng, np, expect);
            CHECK(ng == expect, "group count matches the legacy allocation exactly");
            CHECK(np > 0, "parameter count is non-zero");
            cce_transformer_qat_free(t);
        }
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make qat_block`
Expected: FAIL — `implicit declaration of function 'cce_transformer_qat_group_count'`.

- [ ] **Step 3: Add the registry to the internal header**

In `include/cce/cce_transformer_qat_internal.h`, inside `struct cce_transformer_qat`, add:

```c
    /* Gradcheck group registry. Registration is a SIDE EFFECT of allocation
       (qat_group_add is called by create right where each P is allocated),
       so a new parameter group cannot be added without becoming
       gradcheckable. The previous hand-built array silently dropped
       unregistered groups and had a hard 128 cap. */
    P**  groups;
    const char** group_names;
    int  n_groups, cap_groups;
```

And declare:

```c
void qat_group_reset(cce_transformer_qat* t);
int  qat_group_add(cce_transformer_qat* t, P* p, const char* name);
```

In the public header `include/cce/cce_transformer_qat.h`, add:

```c
/* Introspection for the registration audit: how many parameter groups the
   gradcheck will visit, and how many scalar parameters they hold. */
int cce_transformer_qat_group_count(const cce_transformer_qat* t);
int cce_transformer_qat_param_count(const cce_transformer_qat* t);
```

- [ ] **Step 4: Implement the registry**

In `cce_transformer_qat.c`:

```c
void qat_group_reset(cce_transformer_qat* t) {
    free(t->groups); free((void*)t->group_names);
    t->groups = NULL; t->group_names = NULL;
    t->n_groups = 0; t->cap_groups = 0;
}

int qat_group_add(cce_transformer_qat* t, P* p, const char* name) {
    if (t->n_groups >= t->cap_groups) {
        int nc = t->cap_groups ? t->cap_groups * 2 : 32;
        P** ng = (P**)realloc(t->groups, (size_t)nc * sizeof *ng);
        const char** nn = (const char**)realloc((void*)t->group_names,
                                                (size_t)nc * sizeof *nn);
        if (!ng || !nn) { free(ng); free((void*)nn); return 0; }
        t->groups = ng; t->group_names = nn; t->cap_groups = nc;
    }
    t->groups[t->n_groups] = p;
    t->group_names[t->n_groups] = name;
    t->n_groups++;
    return 1;
}

int cce_transformer_qat_group_count(const cce_transformer_qat* t) {
    return t ? t->n_groups : 0;
}

int cce_transformer_qat_param_count(const cce_transformer_qat* t) {
    int n = 0;
    if (!t) return 0;
    for (int i = 0; i < t->n_groups; ++i)
        n += t->groups[i]->in * t->groups[i]->out;
    return n;
}
```

In `cce_transformer_qat_create`, call `qat_group_add(t, &t->tok_emb, "tok_emb")` immediately after each successful `p_alloc`, for **every** group including `pos_emb`. In `cce_transformer_qat_free`, call `qat_group_reset(t)`.

- [ ] **Step 5: Rewrite the gradcheck to use the registry**

In `cce_transformer_qat_gradcheck`, delete the entire `P* groups[128]; int ng = 0; ...` hand-built block including the `if (ng + 12 > 124) break;` guard, and replace every subsequent use of `groups[g]` / `ng` with `t->groups[g]` / `t->n_groups`. The directional-derivative and per-group max-|g| logic is unchanged.

**Important:** `pos_emb` is frozen FP and must not receive Adam updates, but it IS gradchecked (it has a gradient). Confirm the existing code's treatment of `pos_emb` in the update path is preserved — registration is about gradcheck reachability, not about trainability.

- [ ] **Step 6: Run to verify it passes**

Run: `make qat_block && cat logs/qat_block.log`
Expected: sections 1 and 2 pass; group count equals `2 + 12*n_layer + 4` = 30 for `n_layer=2`.

If the count differs, do NOT change the expected value to match — find which group is unregistered. That mismatch is the bug this task exists to prevent.

- [ ] **Step 7: Commit**

```bash
git add src/cce/cce_transformer_qat.c include/cce/cce_transformer_qat.h include/cce/cce_transformer_qat_internal.h tests/test_qat_block.c
git -c user.email="7261266+Mokason@users.noreply.github.com" commit -m "fix(qat): make gradcheck group registration structural

The gradcheck walked a hand-built P* groups[128] with 'if (ng + 12 > 124)
break;'. Any group not appended was silently ungradchecked, and SwiGLU's
13th per-layer matrix would have broken the bound quietly. Registration is
now a side effect of allocation, with an audit asserting the count.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 3: Config plumbing, validation, and the legacy byte-identity gate

No behaviour change — the contract that zero-init equals today's model, locked by a test before any op is added.

**Files:** Modify `include/cce/cce_transformer_qat.h`, `src/cce/cce_transformer_qat.c`, `tests/test_qat_block.c`

**Interfaces:**
- Produces: enums `QAT_NORM_LN=0`/`QAT_NORM_RMS=1`, `QAT_POS_LEARNED=0`/`QAT_POS_ROPE=1`, `QAT_MLP_GELU=0`/`QAT_MLP_SWIGLU=1`, `QAT_ROPE_HALF=0`/`QAT_ROPE_INTERLEAVED=1`; config fields `norm_kind`, `pos_kind`, `mlp_kind`, `rope_pairing`, `n_kv_head`, `no_bias`, `rope_theta`, `norm_eps`. Every later task reads these.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_qat_block.c`:

```c
    printf("[3] zero-init config is legacy, and invalid configs are refused\n");
    {
        cce_transformer_qat_config c;
        cfg_legacy(&c);
        CHECK(c.norm_kind == QAT_NORM_LN, "zero norm_kind is LayerNorm");
        CHECK(c.pos_kind == QAT_POS_LEARNED, "zero pos_kind is learned");
        CHECK(c.mlp_kind == QAT_MLP_GELU, "zero mlp_kind is GELU");
        CHECK(c.no_bias == 0, "zero no_bias means biases present");
        CHECK(c.n_kv_head == 0, "zero n_kv_head means MHA");

        /* refusal, never clamping */
        { cce_transformer_qat_config b = c; b.n_head = 3;   /* 8 % 3 != 0 */
          CHECK(cce_transformer_qat_create(&b) == NULL, "n_embd % n_head != 0 refused"); }
        { cce_transformer_qat_config b = c; b.n_kv_head = 3; /* 2 % 3 != 0 */
          CHECK(cce_transformer_qat_create(&b) == NULL, "n_head % n_kv_head != 0 refused"); }
        { cce_transformer_qat_config b = c; b.n_kv_head = 4; /* > n_head */
          CHECK(cce_transformer_qat_create(&b) == NULL, "n_kv_head > n_head refused"); }
        { cce_transformer_qat_config b = c; b.pos_kind = QAT_POS_ROPE; b.rope_theta = 0.0f;
          CHECK(cce_transformer_qat_create(&b) == NULL, "ROPE with theta<=0 refused"); }
        { cce_transformer_qat_config b = c; b.norm_kind = 99;
          CHECK(cce_transformer_qat_create(&b) == NULL, "out-of-range norm_kind refused"); }
    }

    printf("[4] legacy logits are bit-identical across the refactor\n");
    {
        cce_transformer_qat_config c;
        cce_transformer_qat *a, *b;
        float *la, *lb;
        int i, same = 1;
        cfg_legacy(&c);
        a = cce_transformer_qat_create(&c);
        b = cce_transformer_qat_create(&c);
        la = (float*)malloc(V_ * sizeof *la);
        lb = (float*)malloc(V_ * sizeof *lb);
        CHECK(a && b && la && lb, "two fixed-seed trainers create");
        if (a && b && la && lb) {
            cce_transformer_qat_logits(a, seq, T_, la);
            cce_transformer_qat_logits(b, seq, T_, lb);
            for (i = 0; i < V_; ++i)
                if (memcmp(&la[i], &lb[i], sizeof(float)) != 0) same = 0;
            CHECK(same, "same seed gives bit-identical legacy logits");
            printf("  info legacy logit[0] = %.9g\n", (double)la[0]);
        }
        free(la); free(lb);
        cce_transformer_qat_free(a); cce_transformer_qat_free(b);
    }
```

**Record the printed `legacy logit[0]` value.** Before starting Task 4, run this and note it; every later task must reproduce the same value. That is the byte-identity anchor.

- [ ] **Step 2: Run to verify it fails**

Run: `make qat_block`
Expected: FAIL — `'QAT_NORM_LN' undeclared`.

- [ ] **Step 3: Add the config surface**

In `include/cce/cce_transformer_qat.h`, above the config struct:

```c
/* Every legacy value is 0, so a memset-zeroed config reproduces exactly the
   GPT-2-shaped model this trainer has always built. That is the
   compatibility contract; tests/test_qat_block.c section 4 enforces it. */
typedef enum { QAT_NORM_LN = 0, QAT_NORM_RMS = 1 } qat_norm_kind;
typedef enum { QAT_POS_LEARNED = 0, QAT_POS_ROPE = 1 } qat_pos_kind;
typedef enum { QAT_MLP_GELU = 0, QAT_MLP_SWIGLU = 1 } qat_mlp_kind;
/* Which dimension pairs rotate together. half-split pairs (i, i+hd/2) and is
   the HF Llama/Qwen convention; interleaved pairs (0,1),(2,3),... They are
   NOT interchangeable — a wrong choice trains fine and matches no reference. */
typedef enum { QAT_ROPE_HALF = 0, QAT_ROPE_INTERLEAVED = 1 } qat_rope_pairing;
```

Append to `cce_transformer_qat_config`:

```c
    /* --- modern-block selectors; all-zero = legacy GPT-2 shape --- */
    int norm_kind;      /* qat_norm_kind */
    int pos_kind;       /* qat_pos_kind */
    int mlp_kind;       /* qat_mlp_kind */
    int rope_pairing;   /* qat_rope_pairing */
    int n_kv_head;      /* 0 => n_head (MHA); else must divide n_head */
    int no_bias;        /* 1 => no qkv/proj/up/down bias. Named negatively so
                           zero-init keeps the legacy meaning (biases present). */
    float rope_theta;   /* required > 0 when pos_kind == QAT_POS_ROPE */
    float norm_eps;     /* 0 => 1e-5 default */
```

- [ ] **Step 4: Implement validation in create**

At the top of `cce_transformer_qat_create`, before any allocation:

```c
    if (!cfg) return NULL;
    if (cfg->n_embd <= 0 || cfg->n_head <= 0 || cfg->n_layer <= 0 ||
        cfg->vocab <= 0 || cfg->block_size <= 0 || cfg->mlp_hidden <= 0)
        return NULL;
    if (cfg->n_embd % cfg->n_head != 0) return NULL;
    if (cfg->norm_kind < QAT_NORM_LN || cfg->norm_kind > QAT_NORM_RMS) return NULL;
    if (cfg->pos_kind  < QAT_POS_LEARNED || cfg->pos_kind  > QAT_POS_ROPE) return NULL;
    if (cfg->mlp_kind  < QAT_MLP_GELU || cfg->mlp_kind  > QAT_MLP_SWIGLU) return NULL;
    if (cfg->rope_pairing < QAT_ROPE_HALF || cfg->rope_pairing > QAT_ROPE_INTERLEAVED)
        return NULL;
    if (cfg->n_kv_head < 0 || cfg->n_kv_head > cfg->n_head) return NULL;
    if (cfg->n_kv_head != 0 && cfg->n_head % cfg->n_kv_head != 0) return NULL;
    if (cfg->pos_kind == QAT_POS_ROPE && !(cfg->rope_theta > 0.0f)) return NULL;
```

Then, after copying cfg into `t->cfg`, normalise the two "0 means default" fields into derived members (NOT back into `t->cfg`, so the config stays a faithful record of what was asked for). Add to the internal struct:

```c
    int   kvh;        /* effective KV heads: cfg.n_kv_head ? : n_head */
    float eps;        /* effective norm epsilon: cfg.norm_eps ? : 1e-5f */
```

and set `t->kvh = cfg->n_kv_head ? cfg->n_kv_head : cfg->n_head;`,
`t->eps = cfg->norm_eps > 0.0f ? cfg->norm_eps : 1e-5f;`.

- [ ] **Step 5: Run to verify it passes**

Run: `make qat_block && cat logs/qat_block.log`
Expected: sections 1–4 pass. Note the `legacy logit[0]` value.

- [ ] **Step 6: Commit**

```bash
git add include/cce/cce_transformer_qat.h include/cce/cce_transformer_qat_internal.h src/cce/cce_transformer_qat.c tests/test_qat_block.c
git -c user.email="7261266+Mokason@users.noreply.github.com" commit -m "feat(qat): modern-block config surface with refusal-not-clamping validation

All legacy values are 0, so a zeroed config reproduces today's GPT-2 shape
exactly — locked by a bit-identity gate before any op changes. Invalid
combinations return NULL from create rather than being clamped, since a
clamped config trains a model that is not the one that was asked for.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 4: RMSNorm

**Files:** Modify `src/cce/cce_transformer_qat.c`, `tests/test_qat_block.c`

**Interfaces:**
- Consumes: `t->cfg.norm_kind`, `t->eps`, `qat_group_add` (Task 2).
- Produces: `rms_fwd`, `rms_bwd` static functions; `norm_fwd`/`norm_bwd` dispatchers used by Tasks 5–8.

### The math

Forward, per position `t` over `D` channels:
`ms = (1/D)·Σ x_i²`, `r = 1/sqrt(ms + eps)`, `y_i = x_i · r · w_i`.

Backward, with `g_i = dy_i · w_i`:
`dw_i += dy_i · x_i · r`
`dx_i = r · (g_i − (r²·x_i / D)·Σ_j g_j·x_j)`

Derivation: `∂r/∂x_i = −r³·x_i/D`, so
`∂L/∂x_i = g_i·r − (r³·x_i/D)·Σ_j g_j x_j`. RMSNorm has **no bias**.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_qat_block.c`:

```c
    printf("[5] RMSNorm gradcheck\n");
    {
        cce_transformer_qat_config c;
        cce_transformer_qat* t;
        cfg_legacy(&c);
        c.norm_kind = QAT_NORM_RMS;
        t = cce_transformer_qat_create(&c);
        CHECK(t != NULL, "RMSNorm trainer creates");
        if (t) {
            /* RMSNorm has no bias: 3 ln biases per layer + lnf_b disappear */
            int expect = 2 + 9 * c.n_layer + 3;
            printf("  info RMS groups=%d expected=%d\n",
                   cce_transformer_qat_group_count(t), expect);
            CHECK(cce_transformer_qat_group_count(t) == expect,
                  "RMSNorm drops the norm bias groups");
            {
                double rel = cce_transformer_qat_gradcheck(t, seq, T_, tgt, 6);
                printf("  info RMSNorm gradcheck rel err = %.3e\n", rel);
                CHECK(rel < 5e-3, "RMSNorm backward matches central differences");
            }
            cce_transformer_qat_free(t);
        }
    }
```

Note the expected group count: legacy has `ln1_w, ln1_b, ln2_w, ln2_b` per layer (4) within its 12; RMS keeps only `ln1_w, ln2_w` (2), so per-layer drops 12→10, and the tail loses `lnf_b`, 4→3. Recompute as `2 + 10*n_layer + 3` if the per-layer accounting differs from the `9` above — **verify against the actual allocation rather than trusting either number**, and set the test to what the allocation genuinely produces once you have confirmed each group is intentional.

- [ ] **Step 2: Run to verify it fails**

Run: `make qat_block`
Expected: FAIL on the group-count check (RMSNorm not implemented, so it still allocates LN biases).

- [ ] **Step 3: Implement RMSNorm forward and backward**

Add next to `ln_fwd`/`ln_bwd`:

```c
/* RMSNorm: no mean subtraction, no bias. rstd[t] cached for backward. */
static void rms_fwd(const float* x, const float* w, int T, int D, float eps,
                    float* y, float* rstd) {
    for (int p = 0; p < T; ++p) {
        const float* xr = x + (size_t)p * D;
        float* yr = y + (size_t)p * D;
        double ms = 0.0;
        for (int i = 0; i < D; ++i) ms += (double)xr[i] * xr[i];
        ms /= (double)D;
        {
            float r = (float)(1.0 / sqrt(ms + (double)eps));
            rstd[p] = r;
            for (int i = 0; i < D; ++i) yr[i] = xr[i] * r * w[i];
        }
    }
}

static void rms_bwd(const float* x, const float* w, const float* dy,
                    const float* rstd, int T, int D,
                    float* dx, float* dw) {
    for (int p = 0; p < T; ++p) {
        const float* xr = x + (size_t)p * D;
        const float* dyr = dy + (size_t)p * D;
        float* dxr = dx + (size_t)p * D;
        float r = rstd[p];
        double gx = 0.0;
        for (int i = 0; i < D; ++i) {
            double g = (double)dyr[i] * w[i];
            gx += g * xr[i];
            if (dw) dw[i] += dyr[i] * xr[i] * r;
        }
        for (int i = 0; i < D; ++i) {
            double g = (double)dyr[i] * w[i];
            dxr[i] = (float)(r * (g - ((double)r * r * xr[i] / (double)D) * gx));
        }
    }
}
```

Add dispatchers that Tasks 5–8 call instead of `ln_fwd`/`ln_bwd` directly:

```c
static void norm_fwd(const cce_transformer_qat* t, const float* x,
                     const float* w, const float* b, int T, int D,
                     float* y, float* mean, float* rstd) {
    if (t->cfg.norm_kind == QAT_NORM_RMS) rms_fwd(x, w, T, D, t->eps, y, rstd);
    else ln_fwd(x, w, b, T, D, y, mean, rstd);
}

static void norm_bwd(const cce_transformer_qat* t, const float* x,
                     const float* w, const float* dy,
                     const float* mean, const float* rstd, int T, int D,
                     float* dx, float* dw, float* db) {
    if (t->cfg.norm_kind == QAT_NORM_RMS)
        rms_bwd(x, w, dy, rstd, T, D, dx, dw);
    else
        ln_bwd(x, w, dy, mean, rstd, T, D, dx, dw, db);
}
```

Match `ln_fwd`/`ln_bwd`'s actual parameter order when writing these — read those two functions first and mirror their signatures exactly.

In `create`, allocate and register `ln1_b`, `ln2_b`, `lnf_b` **only** when `cfg->norm_kind == QAT_NORM_LN`. In `free`, guard the corresponding `p_free`. Replace all `ln_fwd`/`ln_bwd` call sites in `tr_forward`/`tr_backward` with `norm_fwd`/`norm_bwd`.

- [ ] **Step 4: Run to verify it passes**

Run: `make qat_block && cat logs/qat_block.log`
Expected: section 5 passes, and sections 1–4 still pass — including the legacy `logit[0]` matching the value recorded in Task 3.

- [ ] **Step 5: Commit**

```bash
git add src/cce/cce_transformer_qat.c tests/test_qat_block.c
git -c user.email="7261266+Mokason@users.noreply.github.com" commit -m "feat(qat): RMSNorm as a selectable norm op, gradcheck-gated

No mean term and no bias, so the norm-bias groups are not allocated under
QAT_NORM_RMS. Legacy LayerNorm path and its bit-identity anchor unchanged.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 5: RoPE

**Files:** Modify `src/cce/cce_transformer_qat.c`, `tests/test_qat_block.c`

**Interfaces:**
- Consumes: `t->cfg.pos_kind`, `t->cfg.rope_theta`, `t->cfg.rope_pairing`, `t->hd`.
- Produces: `rope_pair(hd, pairing, j, &a, &b)`, `rope_apply`, `rope_bwd`.

### The math

RoPE has **no parameters**. For pair index `j ∈ [0, hd/2)` at position `p`:
`freq = theta^(−2j/hd)`, `ang = p·freq`, `cs = cos(ang)`, `sn = sin(ang)`.

Half-split pairs `(j, j + hd/2)`; interleaved pairs `(2j, 2j+1)`.

Forward: `a' = a·cs − b·sn`, `b' = a·sn + b·cs`
Backward is the transposed (inverse) rotation:
`da = da'·cs + db'·sn`, `db = −da'·sn + db'·cs`

Applied to **Q and K only**, never V. When `pos_kind == QAT_POS_ROPE`, `pos_emb` is not allocated and the embedding step adds no positional term.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_qat_block.c`:

```c
    printf("[6] RoPE: properties then gradcheck\n");
    {
        cce_transformer_qat_config c;
        cce_transformer_qat* t;
        cfg_legacy(&c);
        c.pos_kind = QAT_POS_ROPE;
        c.rope_theta = 10000.0f;
        t = cce_transformer_qat_create(&c);
        CHECK(t != NULL, "RoPE trainer creates");
        if (t) {
            /* pos_emb is not allocated under RoPE */
            int expect = 1 + 12 * c.n_layer + 4;
            printf("  info RoPE groups=%d expected=%d\n",
                   cce_transformer_qat_group_count(t), expect);
            CHECK(cce_transformer_qat_group_count(t) == expect,
                  "RoPE drops the learned pos_emb group");
            {
                double rel = cce_transformer_qat_gradcheck(t, seq, T_, tgt, 6);
                printf("  info RoPE gradcheck rel err = %.3e\n", rel);
                CHECK(rel < 5e-3, "RoPE backward matches central differences");
            }
            cce_transformer_qat_free(t);
        }
    }

    printf("[7] RoPE algebraic properties\n");
    {
        /* norm preservation and relative-position invariance, checked on the
           exported helper so a pairing bug shows up here rather than as a
           mysteriously bad model later. */
        int hd = 8, j;
        float q[8], k[8], qa[8], ka[8], qb[8], kb[8];
        double n0 = 0, n1 = 0, d_far = 0, d_near = 0;
        for (j = 0; j < hd; ++j) { q[j] = 0.1f * (j + 1); k[j] = 0.2f - 0.03f * j; }
        memcpy(qa, q, sizeof q); memcpy(ka, k, sizeof k);
        memcpy(qb, q, sizeof q); memcpy(kb, k, sizeof k);
        cce_transformer_qat_rope_test(qa, hd, 3, 10000.0f, QAT_ROPE_HALF);
        cce_transformer_qat_rope_test(ka, hd, 1, 10000.0f, QAT_ROPE_HALF);
        cce_transformer_qat_rope_test(qb, hd, 5, 10000.0f, QAT_ROPE_HALF);
        cce_transformer_qat_rope_test(kb, hd, 3, 10000.0f, QAT_ROPE_HALF);
        for (j = 0; j < hd; ++j) {
            n0 += (double)q[j] * q[j];
            n1 += (double)qa[j] * qa[j];
            d_far  += (double)qa[j] * ka[j];
            d_near += (double)qb[j] * kb[j];
        }
        CHECK(fabs(n0 - n1) < 1e-6, "rotation preserves vector norm");
        CHECK(fabs(d_far - d_near) < 1e-5,
              "dot product depends only on relative offset (3-1 == 5-3)");
    }
```

This needs a tiny exported test hook in the public header:

```c
/* Test hook: rotate one head-dim vector in place at position p. Exposed so
   the RoPE pairing convention can be checked algebraically — a wrong pairing
   trains fine and matches no reference, so it must be gated directly. */
void cce_transformer_qat_rope_test(float* v, int hd, int pos, float theta,
                                   int pairing);
```

- [ ] **Step 2: Run to verify it fails**

Run: `make qat_block`
Expected: FAIL — `implicit declaration of function 'cce_transformer_qat_rope_test'`.

- [ ] **Step 3: Implement RoPE**

```c
/* Which two head-dim slots rotate together. half-split is the HF Llama/Qwen
   convention and the default; interleaved is the other live convention. */
static void rope_pair(int hd, int pairing, int j, int* a, int* b) {
    if (pairing == QAT_ROPE_INTERLEAVED) { *a = 2 * j; *b = 2 * j + 1; }
    else { *a = j; *b = j + hd / 2; }
}

static void rope_angles(int hd, int pos, float theta, int j,
                        float* cs, float* sn) {
    double freq = pow((double)theta, -2.0 * (double)j / (double)hd);
    double ang = (double)pos * freq;
    *cs = (float)cos(ang);
    *sn = (float)sin(ang);
}

/* In-place rotation of one head-dim vector at position pos. */
static void rope_apply(float* v, int hd, int pos, float theta, int pairing) {
    for (int j = 0; j < hd / 2; ++j) {
        int a, b; float cs, sn, va, vb;
        rope_pair(hd, pairing, j, &a, &b);
        rope_angles(hd, pos, theta, j, &cs, &sn);
        va = v[a]; vb = v[b];
        v[a] = va * cs - vb * sn;
        v[b] = va * sn + vb * cs;
    }
}

/* Transposed rotation: the exact adjoint of rope_apply. */
static void rope_bwd(float* dv, int hd, int pos, float theta, int pairing) {
    for (int j = 0; j < hd / 2; ++j) {
        int a, b; float cs, sn, da, db;
        rope_pair(hd, pairing, j, &a, &b);
        rope_angles(hd, pos, theta, j, &cs, &sn);
        da = dv[a]; db = dv[b];
        dv[a] =  da * cs + db * sn;
        dv[b] = -da * sn + db * cs;
    }
}

void cce_transformer_qat_rope_test(float* v, int hd, int pos, float theta,
                                   int pairing) {
    rope_apply(v, hd, pos, theta, pairing);
}
```

Wiring: in `tr_forward`, after the QKV projection and before attention, when `t->cfg.pos_kind == QAT_POS_ROPE`, call `rope_apply` on each head's Q slice and each head's K slice at that head's position. In `tr_backward`, call `rope_bwd` on the corresponding dQ and dK slices at the **same** point in the reverse order (i.e. after attention backward produces dQ/dK, before the QKV linear backward). Do **not** rotate V.

In `create`, skip allocating/registering `pos_emb` when `pos_kind == QAT_POS_ROPE`; in the embedding step, skip the positional add. In `free`, guard the `p_free`.

- [ ] **Step 4: Run to verify it passes**

Run: `make qat_block && cat logs/qat_block.log`
Expected: sections 6 and 7 pass; sections 1–5 still pass with the legacy anchor unchanged.

- [ ] **Step 5: Commit**

```bash
git add include/cce/cce_transformer_qat.h src/cce/cce_transformer_qat.c tests/test_qat_block.c
git -c user.email="7261266+Mokason@users.noreply.github.com" commit -m "feat(qat): RoPE with pinned half-split pairing, gradcheck + property gated

No parameters; backward is the exact transposed rotation. Pairing is pinned
to half-split (HF Llama/Qwen) and switchable, gated algebraically by norm
preservation and relative-position invariance — a wrong pairing trains fine
and matches no reference, so gradcheck alone would not catch it.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 6: GQA

The highest-risk task: the KV gradient must accumulate across every query head that read it.

**Files:** Modify `src/cce/cce_transformer_qat.c`, `include/cce/cce_transformer_qat_internal.h`, `tests/test_qat_block.c`

**Interfaces:**
- Consumes: `t->kvh` (Task 3).
- Produces: split `q_w/q_b`, `k_w/k_b`, `v_w/v_b` replacing fused `qkv_w/qkv_b`; caches `qc`, `kc`, `vc`.

### The shape change

Fused `qkv_w [D][3D]` becomes `q_w [D][D]`, `k_w [D][kvh·hd]`, `v_w [D][kvh·hd]`.
Query head `h` reads KV head `h / (n_head / kvh)`.

**This applies to the legacy path too** — under MHA, `kvh == n_head`, so
`k_w` and `v_w` are `[D][D]` and the three matrices concatenate to exactly
the old fused layout. Task 3's bit-identity gate is what proves the split did
not change legacy numerics; if it goes red here, the split is wrong, not the
gate.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_qat_block.c`:

```c
    printf("[8] GQA gradcheck and KV gradient accumulation\n");
    {
        cce_transformer_qat_config c;
        cce_transformer_qat* t;
        cfg_legacy(&c);
        c.n_head = 4; c.n_embd = 8;   /* hd = 2 */
        c.n_kv_head = 2;              /* 2 query heads share each KV head */
        t = cce_transformer_qat_create(&c);
        CHECK(t != NULL, "GQA trainer creates");
        if (t) {
            double rel = cce_transformer_qat_gradcheck(t, seq, T_, tgt, 6);
            printf("  info GQA gradcheck rel err = %.3e\n", rel);
            CHECK(rel < 5e-3, "GQA backward matches central differences");
            cce_transformer_qat_free(t);
        }
    }

    printf("[9] MHA is the kvh == n_head special case\n");
    {
        cce_transformer_qat_config c1, c2;
        cce_transformer_qat *a, *b;
        float *la, *lb;
        int i, same = 1;
        cfg_legacy(&c1);
        cfg_legacy(&c2); c2.n_kv_head = c2.n_head;   /* explicit MHA */
        a = cce_transformer_qat_create(&c1);
        b = cce_transformer_qat_create(&c2);
        la = (float*)malloc(V_ * sizeof *la);
        lb = (float*)malloc(V_ * sizeof *lb);
        CHECK(a && b && la && lb, "both trainers create");
        if (a && b && la && lb) {
            cce_transformer_qat_logits(a, seq, T_, la);
            cce_transformer_qat_logits(b, seq, T_, lb);
            for (i = 0; i < V_; ++i)
                if (memcmp(&la[i], &lb[i], sizeof(float)) != 0) same = 0;
            CHECK(same, "n_kv_head=0 and n_kv_head=n_head are bit-identical");
        }
        free(la); free(lb);
        cce_transformer_qat_free(a); cce_transformer_qat_free(b);
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make qat_block`
Expected: FAIL — the GQA gradcheck exceeds tolerance, or create returns NULL, because the fused QKV cannot express `kvh < n_head`.

- [ ] **Step 3: Split QKV and implement GQA**

In the internal struct, replace `P *qkv_w, *qkv_b;` with:

```c
    P *q_w, *q_b;     /* per layer [D][D],       [1][D]       */
    P *k_w, *k_b;     /* per layer [D][kvh*hd],  [1][kvh*hd]  */
    P *v_w, *v_b;     /* per layer [D][kvh*hd],  [1][kvh*hd]  */
```

and replace the `qkv` cache with `qc [L][T][D]`, `kc [L][T][kvh*hd]`, `vc [L][T][kvh*hd]`, plus `dqc/dkc/dvc` backward scratch replacing `dqkv`.

In `tr_forward`, run three `lin_fwd` calls instead of one, then index attention as:

```c
    int gsz = t->cfg.n_head / t->kvh;    /* query heads per KV head */
    /* for query head h: */
    int kvh_idx = h / gsz;
    const float* krow = kc + ((size_t)l * T + p) * (t->kvh * t->hd)
                           + (size_t)kvh_idx * t->hd;
```

In `tr_backward`, the critical part — accumulate into the shared KV head rather than assigning:

```c
    /* Every query head in this group read the SAME k/v slice, so their
       gradients SUM. Using assignment here silently keeps only the last
       query head's contribution and still produces a plausible-looking
       model; gate [10] checks this directly. */
    float* dkrow = dkc + ((size_t)p) * (t->kvh * t->hd)
                       + (size_t)kvh_idx * t->hd;
    for (int i = 0; i < t->hd; ++i) dkrow[i] += contrib[i];
```

Ensure `dkc`/`dvc` are zeroed once per layer before the head loop, not per head.

Register `q_w,q_b,k_w,k_b,v_w,v_b` with `qat_group_add` in place of the old `qkv_w,qkv_b`. Update the legacy group-count expectations in sections 2–7 of the test from 12 to 16 per layer accordingly (2 groups become 6), and re-verify each count against the actual allocation.

- [ ] **Step 4: Run to verify it passes**

Run: `make qat_block && cat logs/qat_block.log`
Expected: sections 8 and 9 pass. **Section 4's legacy `logit[0]` must still match the Task 3 value** — that is the proof the QKV split preserved legacy numerics.

- [ ] **Step 5: Add the direct accumulation check**

Append:

```c
    printf("[10] KV gradient equals the sum over sharing query heads\n");
    {
        /* With gsz query heads per KV head, scaling every query head's
           contribution must scale the KV gradient by the same factor. An
           assignment bug (last-writer-wins) would make the KV gradient
           independent of gsz. Compare kvh=1 (all heads share) against
           kvh=n_head (none share) on the same seed: the shared case must
           have a strictly larger KV gradient norm. */
        cce_transformer_qat_config c1, c2;
        cce_transformer_qat *a, *b;
        double na, nb;
        cfg_legacy(&c1); c1.n_head = 4; c1.n_embd = 8; c1.n_kv_head = 1;
        cfg_legacy(&c2); c2.n_head = 4; c2.n_embd = 8; c2.n_kv_head = 4;
        a = cce_transformer_qat_create(&c1);
        b = cce_transformer_qat_create(&c2);
        CHECK(a && b, "both GQA configs create");
        if (a && b) {
            na = cce_transformer_qat_kgrad_norm(a, seq, T_, tgt);
            nb = cce_transformer_qat_kgrad_norm(b, seq, T_, tgt);
            printf("  info |dK| shared=%.6g unshared=%.6g\n", na, nb);
            CHECK(na > 0.0 && nb > 0.0, "both produce non-zero K gradient");
            CHECK(na != nb, "sharing changes the K gradient (not last-writer-wins)");
        }
        cce_transformer_qat_free(a); cce_transformer_qat_free(b);
    }
```

Add the hook to the public header and implement it as: run forward, build `dlogits` as in gradcheck, run `tr_zero_grads` + `tr_backward`, then return `sqrt(Σ k_w[0].g²)`.

```c
/* Test hook: L2 norm of layer-0 k_w gradient after one backward. Exposed so
   GQA's KV accumulation can be checked directly — a last-writer-wins bug can
   cancel in the directional gradcheck projection. */
double cce_transformer_qat_kgrad_norm(cce_transformer_qat* t, const int* tokens,
                                      int T, int target);
```

- [ ] **Step 6: Run and commit**

Run: `make qat_block && cat logs/qat_block.log`
Expected: section 10 passes.

```bash
git add include/cce/cce_transformer_qat.h include/cce/cce_transformer_qat_internal.h src/cce/cce_transformer_qat.c tests/test_qat_block.c
git -c user.email="7261266+Mokason@users.noreply.github.com" commit -m "feat(qat): GQA via split Q/K/V, with a direct KV-accumulation gate

Fused qkv [D][3D] splits into q [D][D] and k/v [D][kvh*hd]. Under MHA the
three concatenate to the old layout, proven by the legacy bit-identity anchor
holding across the split.

KV gradients SUM across every query head that read them. Assignment there is
last-writer-wins and can cancel in the directional gradcheck projection, so
it gets its own direct check rather than relying on gradcheck alone.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 7: SwiGLU

**Files:** Modify `src/cce/cce_transformer_qat.c`, `include/cce/cce_transformer_qat_internal.h`, `tests/test_qat_block.c`

**Interfaces:**
- Consumes: `t->cfg.mlp_kind`.
- Produces: `gate_w/gate_b` per layer; `mgate` cache; `silu`/`silu_d`.

### The math

`silu(x) = x·σ(x)`; `silu'(x) = σ(x)·(1 + x·(1 − σ(x)))`

Forward: `gp = x@gate_w`, `up = x@up_w`, `h = silu(gp)·up`, `y = h@down_w`
Backward, given `dh`:
`d_up = dh · silu(gp)`
`d_gp = dh · up · silu'(gp)`

- [ ] **Step 1: Write the failing test**

```c
    printf("[11] SwiGLU gradcheck\n");
    {
        cce_transformer_qat_config c;
        cce_transformer_qat* t;
        cfg_legacy(&c);
        c.mlp_kind = QAT_MLP_SWIGLU;
        t = cce_transformer_qat_create(&c);
        CHECK(t != NULL, "SwiGLU trainer creates");
        if (t) {
            double rel = cce_transformer_qat_gradcheck(t, seq, T_, tgt, 6);
            printf("  info SwiGLU groups=%d gradcheck rel err = %.3e\n",
                   cce_transformer_qat_group_count(t), rel);
            CHECK(rel < 5e-3, "SwiGLU backward matches central differences");
            cce_transformer_qat_free(t);
        }
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make qat_block`
Expected: FAIL — SwiGLU unimplemented, so the gradcheck runs the GELU path and the config is silently ignored (or the group count is unchanged).

- [ ] **Step 3: Implement SwiGLU**

```c
static float silu_f(float x) {
    double s = 1.0 / (1.0 + exp(-(double)x));
    return (float)((double)x * s);
}

static float silu_df(float x) {
    double s = 1.0 / (1.0 + exp(-(double)x));
    return (float)(s * (1.0 + (double)x * (1.0 - s)));
}
```

Add `P *gate_w, *gate_b;` per layer and `float* mgate;` cache `[L][T][M]`,
allocated and registered only when `mlp_kind == QAT_MLP_SWIGLU`.

In `tr_forward`'s MLP section, branch: GELU keeps the existing path; SwiGLU
computes `mgate` from `gate_w`, `mpre` from `up_w`, then
`mpost[i] = silu_f(mgate[i]) * mpre[i]`.

In `tr_backward`, given `dmpost`:
```c
    /* product rule across the two branches */
    d_up[i]   = dmpost[i] * silu_f(mgate[i]);
    d_gate[i] = dmpost[i] * mpre[i] * silu_df(mgate[i]);
```
then propagate `d_up` through `up_w` and `d_gate` through `gate_w`, summing
both contributions into `dx`.

- [ ] **Step 4: Run and commit**

Run: `make qat_block && cat logs/qat_block.log`
Expected: section 11 passes; the legacy anchor still matches.

```bash
git add include/cce/cce_transformer_qat_internal.h src/cce/cce_transformer_qat.c tests/test_qat_block.c
git -c user.email="7261266+Mokason@users.noreply.github.com" commit -m "feat(qat): SwiGLU MLP as a selectable op, gradcheck-gated

Adds a third per-layer matrix (gate_w) and a product rule across the two
branches. The 13th per-layer group is exactly what the old groups[128] /
'ng + 12 > 124' guard would have silently dropped — the Task 2 registry is
what makes this safe to add.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Task 8: Optional bias, and the full modern endpoint

**Files:** Modify `src/cce/cce_transformer_qat.c`, `tests/test_qat_block.c`

- [ ] **Step 1: Write the failing test**

```c
    printf("[12] no-bias, then the full modern endpoint\n");
    {
        cce_transformer_qat_config c;
        cce_transformer_qat* t;
        cfg_legacy(&c);
        c.no_bias = 1;
        t = cce_transformer_qat_create(&c);
        CHECK(t != NULL, "no-bias trainer creates");
        if (t) {
            double rel = cce_transformer_qat_gradcheck(t, seq, T_, tgt, 6);
            printf("  info no-bias groups=%d rel err = %.3e\n",
                   cce_transformer_qat_group_count(t), rel);
            CHECK(rel < 5e-3, "no-bias backward matches central differences");
            cce_transformer_qat_free(t);
        }

        /* the endpoint this whole spec exists for */
        cfg_legacy(&c);
        c.n_head = 4; c.n_embd = 8;
        c.norm_kind = QAT_NORM_RMS;
        c.pos_kind  = QAT_POS_ROPE;   c.rope_theta = 10000.0f;
        c.mlp_kind  = QAT_MLP_SWIGLU;
        c.n_kv_head = 2;
        c.no_bias   = 1;
        t = cce_transformer_qat_create(&c);
        CHECK(t != NULL, "full modern (Llama-shaped) trainer creates");
        if (t) {
            double rel = cce_transformer_qat_gradcheck(t, seq, T_, tgt, 6);
            printf("  info MODERN groups=%d gradcheck rel err = %.3e\n",
                   cce_transformer_qat_group_count(t), rel);
            CHECK(rel < 5e-3, "modern-endpoint backward matches central differences");
            {
                double l0 = cce_transformer_qat_step(t, seq, T_, NULL, tgt, 1e-3f);
                double l1 = l0;
                for (int it = 0; it < 40; ++it)
                    l1 = cce_transformer_qat_step(t, seq, T_, NULL, tgt, 1e-3f);
                printf("  info modern loss %.4f -> %.4f\n", l0, l1);
                CHECK(l1 < l0, "modern block actually trains (loss falls)");
            }
            cce_transformer_qat_free(t);
        }
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make qat_block`
Expected: FAIL — `no_bias` is not honoured, so the group count is unchanged and/or the modern endpoint does not create.

- [ ] **Step 3: Implement optional bias**

In `create`, allocate and register `q_b`, `k_b`, `v_b`, `proj_b`, `up_b`,
`down_b`, `gate_b` only when `cfg->no_bias == 0`. Guard `p_free` in `free`.

In `tr_forward`/`tr_backward`, pass `NULL` for the bias pointer to `lin_fwd`
and `lin_bwd` when `no_bias` is set. Confirm both already accept a `NULL`
bias by reading them; if `lin_fwd` dereferences `b` unconditionally, add the
guard there rather than at every call site.

`head_b` and the norm weights are unaffected — modern models keep the norm
weight, and dropping the head bias is not part of this spec.

- [ ] **Step 4: Run and commit**

Run: `make qat_block && cat logs/qat_block.log && make attribution && make acquire`
Expected: all 12 sections pass; the attribution gate still passes; the acquire gate is unchanged from its pre-existing state.

```bash
git add src/cce/cce_transformer_qat.c tests/test_qat_block.c
git -c user.email="7261266+Mokason@users.noreply.github.com" commit -m "feat(qat): optional bias, and the full Llama-shaped modern endpoint

RMSNorm + RoPE + GQA + SwiGLU + no-bias compose and gradcheck together, and
the composed block trains. Zero-init config still reproduces the legacy
GPT-2 shape bit-identically.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| §1.1 `groups[]` hole | Task 2 |
| §2 config table, zero-init contract | Task 3 |
| §2.0 RoPE pairing pinned + switchable | Task 3 (enum), Task 5 (impl + property gate) |
| §2.1 RMSNorm | Task 4 |
| §2.1 RoPE | Task 5 |
| §2.1 SwiGLU | Task 7 |
| §2.1 GQA + accumulation risk | Task 6 (steps 3, 5) |
| §2.1 no-bias | Task 8 |
| §2.2 cache changes | Task 6 (q/k/v), Task 7 (mgate) |
| §3 loader split | Task 1 |
| §4 refusal-not-clamping | Task 3 step 4 |
| §5 what does not change | Global Constraints |
| §6 gate 1 registration audit | Task 2 |
| §6 gate 2 progressive gradcheck | Tasks 4–8 |
| §6 gate 3 GQA accumulation | Task 6 step 5 |
| §6 gate 4 legacy byte-identity | Task 3 step 1, re-checked in Tasks 4–8 |
| §6 gate 5 RoPE properties | Task 5 step 1 |
| §6 gate 6 existing gates | Task 8 step 4 (Linux-only, stated) |

No gaps.

**Placeholder scan:** No TBD/TODO. Several steps instruct verification against the real source before writing (`ln_fwd` signatures, `lin_fwd` NULL-bias handling, `CCE :=` location, group-count arithmetic). Each names the exact file and what to look for. The group-count expectations in Tasks 4–6 are explicitly flagged as *to be verified against the actual allocation, not trusted* — because a test tuned to match a buggy allocation is worse than no test.

**Type consistency:** `qat_group_add`/`qat_group_reset`/`cce_transformer_qat_group_count` are used identically in Tasks 2–8. `norm_fwd`/`norm_bwd` (Task 4) are the dispatchers Tasks 5–8 call. `t->kvh` and `t->eps` are introduced in Task 3 and consumed in Tasks 4 and 6. `rope_pair` signature matches between Task 5's implementation and its test hook.

**Known risks, flagged not hidden:**
1. Task 6 changes the legacy path (QKV split) and is the one place the byte-identity anchor could legitimately go red. If it does, the split is wrong — do not relax the gate.
2. Group-count arithmetic in Tasks 4–6 is stated approximately and must be reconciled against the real allocation; the plan says so at each site rather than asserting numbers it has not verified.
3. Task 1 step 4 assumes `$(CCE)` is a plain source list. If it is an object list, the loader must be added wherever those objects are produced instead.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-08-16-qat-modern-block.md`.
