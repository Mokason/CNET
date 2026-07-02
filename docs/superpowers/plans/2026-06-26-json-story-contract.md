# JSON-as-Contract over TinyStories — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a self-contained demo that *generates* a well-formed JSON object `{"title","characters","story"}` whose story is LM-generated, and *parses* it back losslessly, with the JSON escaping/un-escaping decisions PROVEN (exhaustive certification) and the `characters` name-gate certified + conformal.

**Architecture:** JSON well-formedness reduces to a per-byte escape decision over an enumerable 256-byte domain → `btn_certify_exhaustive` gives a PROOF, not a sample. The fixed schema skeleton is well-formed by construction once every value is escaped. The story text comes from the existing self-contained `cce_wordlm`; the `characters` field reuses the endgate name-gate pattern (certify + conformal abstention). One new demo `tests/jsonstory_demo.c`, one new oracle header `tests/json_validate.h`, one new `make jsonstory` target. Zero edits to core/router/CCE sources.

**Tech Stack:** C11 (gcc, `-mno-avx` per repo CFLAGS), `nn.h` (BTN), `contract/contract.h`, `contract/coverage.h` (exhaustive certify), `contract/conformal.h`, `cce/cce_wordlm.h` (story LM). Independent oracle: the recursive-descent validator extracted from `tests/cce_json_bench.c`.

**Project note — NO GIT:** This repo's `.git` was removed. Run **no** git commands. "Commit" steps are replaced by a **checkpoint**: re-run `make jsonstory` and confirm all prior assertions still pass. Verify state via the filesystem.

---

## File Structure

- **Create `tests/json_validate.h`** — single-header, all-`static` recursive-descent JSON validator (`json_is_valid`), extracted verbatim from `tests/cce_json_bench.c`. Independent oracle of well-formedness. ~140 lines.
- **Create `tests/jsonstory_demo.c`** — the demo. File-scope helpers + a `main()` that calls staged section functions and aggregates a single pass flag (returns 0 = all PASS, 1 = any FAIL).
- **Modify `Makefile`** — add `JSONSTORY_DEMO` var, the `jsonstory` target, `jsonstory` in `.PHONY`, and `jsonstory` to the `clean` rm list.

`cce_json_bench.c` is **not** modified.

---

## Task 1: Oracle header + demo scaffold + make target

**Files:**
- Create: `tests/json_validate.h`
- Create: `tests/jsonstory_demo.c`
- Modify: `Makefile` (vars near line 114, target after the `endgate` target ~line 486, `.PHONY` line 117, `clean` rm list ~line 570)

- [ ] **Step 1: Create the validator oracle header by extraction**

Create `tests/json_validate.h`. It is a verbatim lift of the recursive-descent validator already in `tests/cce_json_bench.c` (all functions are already `static`). Paste, **in this order**, the bodies from `cce_json_bench.c`:

1. forward decls (lines 82–83): `skip_ws_json`, `parse_value_json`
2. `skip_ws_json` (lines 217–220)
3. `parse_string_json` (lines 221–254)
4. forward decl (line 255): `parse_value_json`
5. `parse_number_json` (lines 257–287)
6. `parse_array_json` (lines 288–309)
7. `parse_object_json` (lines 310–335)
8. `parse_value_json` (lines 337–346)
9. `json_is_valid` (lines 693–701)

Wrap in an include guard and the needed std headers:

```c
#ifndef JSON_VALIDATE_H
#define JSON_VALIDATE_H
/* Independent JSON well-formedness oracle. Verbatim recursive-descent validator
   lifted from tests/cce_json_bench.c (all functions static). Used to confirm
   the contract-built JSON is well-formed by a parser that knows nothing about
   how it was produced. */
#include <stdbool.h>
#include <string.h>

/* <-- paste items 1..9 above, in order, here --> */

#endif /* JSON_VALIDATE_H */
```

Do not edit `cce_json_bench.c`. (Duplication is intentional: the demo stays self-contained.)

- [ ] **Step 2: Create the demo scaffold with an oracle smoke check**

Create `tests/jsonstory_demo.c`:

```c
/* JSON-as-contract over TinyStories.
   Generates {"title","characters","story"} with an LM-generated story and parses
   it back losslessly. JSON escape/un-escape decisions are PROVEN (exhaustive
   certification); the characters name-gate is certified + conformal (endgate
   pattern). The story text comes from the self-contained cce_wordlm. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <assert.h>

#include "../include/nn.h"
#include "../include/contract/contract.h"
#include "../include/contract/coverage.h"
#include "../include/contract/conformal.h"
#include "../include/cce/cce_wordlm.h"
#include "json_validate.h"

/* ---- Task 1: oracle smoke ------------------------------------------------ */
static int oracle_smoke(void) {
    int ok = 1;
    ok &= (json_is_valid("{\"a\":\"b\",\"c\":[\"d\",\"e\"]}") == true);
    ok &= (json_is_valid("{\"a\":}") == false);
    ok &= (json_is_valid("not json") == false);
    printf("[oracle] json_is_valid smoke: %s\n", ok ? "ok" : "FAIL");
    return ok;
}

int main(void) {
    srand(7);
    printf("=== JSON-as-contract over TinyStories ===\n");
    int ok = 1;
    ok &= oracle_smoke();
    printf(ok ? "\nALL PASS\n" : "\nFAIL\n");
    return ok ? 0 : 1;
}
```

- [ ] **Step 3: Add the Makefile var, target, PHONY, clean**

In `Makefile`, near the other demo vars (around line 114), add:

```make
JSONSTORY_DEMO := tests/jsonstory_demo.c
```

After the `endgate` target (around line 486), add:

```make
# JSON-as-contract over TinyStories: generate {"title","characters","story"} with
# an LM-generated story + parse it back losslessly. Escape/un-escape decisions are
# PROVEN (btn_certify_exhaustive); characters name-gate is certified + conformal.
jsonstory: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(CONFORMAL) $(CCE_WORDLM) $(JSONSTORY_DEMO) include/nn.h include/contract/contract.h include/contract/coverage.h include/contract/conformal.h include/cce/cce_wordlm.h tests/json_validate.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(PROPERTY) $(CONSOLIDATE) $(SCAN) $(COVERAGE) $(CONFORMAL) $(CCE_WORDLM) $(JSONSTORY_DEMO) $(LDFLAGS)
	./jsonstory
```

Add `jsonstory` to the `.PHONY` line (line 117). Add `jsonstory` to the `clean` rm list (the line containing `cce_json_bench cce_smoke *.exe`, ~line 570).

- [ ] **Step 4: Build and run**

Run: `make jsonstory`
Expected: compiles clean; output contains `[oracle] json_is_valid smoke: ok` and `ALL PASS`; exit code 0.

- [ ] **Step 5: Checkpoint (no git)**

Re-run `make jsonstory`; confirm `ALL PASS`.

---

## Task 2: `json_escape_class` — trained + PROVEN (the headline)

**Files:**
- Modify: `tests/jsonstory_demo.c`

- [ ] **Step 1: Add the escape-class reference rule and class enum (above `main`)**

```c
/* ---- Task 2: json_escape_class (byte -> escape class), PROVEN ------------ */
enum { ESC_PASS=0, ESC_QUOTE, ESC_BSLASH, ESC_BS, ESC_TAB, ESC_NL, ESC_FF, ESC_CR, ESC_UNI, ESC_NCLASS };

/* The exact, total escape decision over all 256 bytes. Labels training only. */
static int ref_escape_class(unsigned char b) {
    switch (b) {
        case '"':  return ESC_QUOTE;
        case '\\': return ESC_BSLASH;
        case 0x08: return ESC_BS;   /* \b */
        case 0x09: return ESC_TAB;  /* \t */
        case 0x0A: return ESC_NL;   /* \n */
        case 0x0C: return ESC_FF;   /* \f */
        case 0x0D: return ESC_CR;   /* \r */
    }
    if (b < 0x20 || b >= 0x7F) return ESC_UNI;   /* controls + DEL + high -> \u00XX */
    return ESC_PASS;                              /* 0x20..0x7E except " and \ */
}

/* The frozen primitive + its borrowed training tables (kept alive for the
   demo's lifetime; intentionally not freed). */
static BinaryTransformNetwork g_esc;
static Contract               g_esc_c;

/* argmax of the 9 escape-class outputs of the frozen net (the PROVEN decision). */
static int escape_class_btn(unsigned char b) {
    double in[256]; memset(in, 0, sizeof(in)); in[b] = 1.0;
    const double *o = btn_forward(&g_esc, in);
    int best = 0; for (int k = 1; k < ESC_NCLASS; k++) if (o[k] > o[best]) best = k;
    return best;
}
```

- [ ] **Step 2: Add the build+certify section function (above `main`)**

```c
static int build_escape(void) {
    const size_t IN = 256, OUT = ESC_NCLASS, N = 256;
    double *X = (double*)calloc(N*IN, sizeof(double));
    double *Y = (double*)calloc(N*OUT, sizeof(double));
    for (size_t b = 0; b < N; b++) { X[b*IN + b] = 1.0; Y[b*OUT + ref_escape_class((unsigned char)b)] = 1.0; }

    if (btn_init(&g_esc, IN, OUT, 32, 320, 0.05, 1234u) != 0) { printf("[escape] btn_init failed\n"); return 0; }
    Port pin  = { PORT_ONEHOT, IN,  1, "" }; port_set_tag(&pin,  "json_byte");
    Port pout = { PORT_ONEHOT, OUT, 1, "" }; port_set_tag(&pout, "escape_class");
    btn_set_ports(&g_esc, pin, pout);
    btn_train(&g_esc, X, Y, N, 600);

    if (contract_init_borrowed(&g_esc_c, "json_escape_class", &g_esc, X, Y, N) != 0) {
        printf("[escape] contract_init_borrowed failed\n"); return 0;
    }
    ExhaustiveReport er; memset(&er, 0, sizeof(er));
    int proven = btn_certify_exhaustive(&g_esc, &g_esc_c, 0, &er);
    int ok = (proven == 0) && (er.verdict == CERT_PROVEN) && (er.coverage.kind == COVERAGE_EXHAUSTIVE);
    printf("[escape] json_escape_class: %s  (verdict=%d, domain=%zu, swept=%zu, illformed=%zu)\n",
           ok ? "CERT_PROVEN" : "NOT PROVEN", (int)er.verdict,
           er.coverage.domain_cardinality, er.domain_swept, er.domain_illformed);
    /* tables X,Y are borrowed by g_esc_c; keep them alive (demo leaks). */
    return ok;
}
```

- [ ] **Step 3: Call it from `main` (after `oracle_smoke`)**

```c
    ok &= build_escape();
```

- [ ] **Step 4: Build and run**

Run: `make jsonstory`
Expected: output contains `[escape] json_escape_class: CERT_PROVEN  (verdict=1, domain=256, swept=256, illformed=0)` (verdict 1 = `CERT_PROVEN`); still `ALL PASS`.

If `NOT PROVEN`: raise `max_hidden` in `btn_init` (320 → 512) and/or epochs (600 → 1000); a one-hot→class map is exactly memorizable, so PROVEN is reachable.

- [ ] **Step 5: Checkpoint (no git)** — re-run `make jsonstory`; confirm `ALL PASS`.

---

## Task 3: `json_unescape_short` — trained + PROVEN (the understand side)

**Files:**
- Modify: `tests/jsonstory_demo.c`

- [ ] **Step 1: Add the short-escape alphabet + reference inverse + frozen primitive (above `main`)**

```c
/* ---- Task 3: json_unescape_short (escape letter -> byte), PROVEN --------- */
/* The 7 short escape letters our renderer emits, in a fixed index order. */
static const char ESC_LETTERS[7] = { '"', '\\', 'b', 'f', 'n', 'r', 't' };
static const unsigned char ESC_LETTER_BYTE[7] = { 0x22, 0x5C, 0x08, 0x0C, 0x0A, 0x0D, 0x09 };
static int esc_letter_index(char c) { for (int i = 0; i < 7; i++) if (ESC_LETTERS[i] == c) return i; return -1; }

static BinaryTransformNetwork g_unesc;
static Contract               g_unesc_c;

/* PROVEN decision: escape-letter one-hot -> original byte one-hot (argmax). */
static unsigned char unescape_short_btn(char letter) {
    int idx = esc_letter_index(letter);
    double in[7]; memset(in, 0, sizeof(in)); if (idx >= 0) in[idx] = 1.0;
    const double *o = btn_forward(&g_unesc, in);
    int best = 0; for (int k = 1; k < 256; k++) if (o[k] > o[best]) best = k;
    return (unsigned char)best;
}

static int build_unescape(void) {
    const size_t IN = 7, OUT = 256, N = 7;
    double *X = (double*)calloc(N*IN, sizeof(double));
    double *Y = (double*)calloc(N*OUT, sizeof(double));
    for (size_t i = 0; i < N; i++) { X[i*IN + i] = 1.0; Y[i*OUT + ESC_LETTER_BYTE[i]] = 1.0; }

    if (btn_init(&g_unesc, IN, OUT, 16, 64, 0.05, 4321u) != 0) { printf("[unescape] btn_init failed\n"); return 0; }
    Port pin  = { PORT_ONEHOT, IN,  1, "" }; port_set_tag(&pin,  "escape_letter");
    Port pout = { PORT_ONEHOT, OUT, 1, "" }; port_set_tag(&pout, "json_byte");
    btn_set_ports(&g_unesc, pin, pout);
    btn_train(&g_unesc, X, Y, N, 800);

    if (contract_init_borrowed(&g_unesc_c, "json_unescape_short", &g_unesc, X, Y, N) != 0) {
        printf("[unescape] contract_init_borrowed failed\n"); return 0;
    }
    ExhaustiveReport er; memset(&er, 0, sizeof(er));
    int proven = btn_certify_exhaustive(&g_unesc, &g_unesc_c, 0, &er);
    int ok = (proven == 0) && (er.verdict == CERT_PROVEN) && (er.coverage.kind == COVERAGE_EXHAUSTIVE);
    printf("[unescape] json_unescape_short: %s  (domain=%zu, swept=%zu)\n",
           ok ? "CERT_PROVEN" : "NOT PROVEN", er.coverage.domain_cardinality, er.domain_swept);
    return ok;
}
```

- [ ] **Step 2: Call it from `main` (after `build_escape`)**

```c
    ok &= build_unescape();
```

- [ ] **Step 3: Build and run**

Run: `make jsonstory`
Expected: output contains `[unescape] json_unescape_short: CERT_PROVEN  (domain=7, swept=7)`; still `ALL PASS`.

- [ ] **Step 4: Checkpoint (no git)** — re-run `make jsonstory`; confirm `ALL PASS`.

---

## Task 4: `render` + `parse_one` + round-trip 256/256 + adversarial validity

**Files:**
- Modify: `tests/jsonstory_demo.c`

- [ ] **Step 1: Add the renderer, the single-char parser, and a string reader (above `main`)**

```c
/* ---- Task 4: render / parse_one / round-trip ---------------------------- */
static char hexdig(int v) { return (char)(v < 10 ? '0' + v : 'a' + (v - 10)); }

/* Append the JSON-string encoding of byte b to out (using the PROVEN escape
   decision). out must have room for >= 6 chars + nul. */
static void render_byte(unsigned char b, char *out) {
    int cls = escape_class_btn(b);
    switch (cls) {
        case ESC_PASS:   out[0]=(char)b; out[1]=0; return;
        case ESC_QUOTE:  strcpy(out, "\\\""); return;
        case ESC_BSLASH: strcpy(out, "\\\\"); return;
        case ESC_BS:     strcpy(out, "\\b");  return;
        case ESC_TAB:    strcpy(out, "\\t");  return;
        case ESC_NL:     strcpy(out, "\\n");  return;
        case ESC_FF:     strcpy(out, "\\f");  return;
        case ESC_CR:     strcpy(out, "\\r");  return;
        default:         out[0]='\\'; out[1]='u'; out[2]='0'; out[3]='0';
                         out[4]=hexdig(b>>4); out[5]=hexdig(b&0xF); out[6]=0; return;
    }
}

/* Read ONE logical content byte from s at *i (advancing *i). Mirrors render. */
static unsigned char parse_one(const char *s, int *i) {
    if (s[*i] != '\\') { unsigned char b = (unsigned char)s[*i]; (*i)++; return b; }
    char c = s[*i + 1];
    if (c == 'u') {
        int hi = s[*i+4], lo = s[*i+5];
        int hv = (hi<='9'?hi-'0':(tolower(hi)-'a'+10));
        int lv = (lo<='9'?lo-'0':(tolower(lo)-'a'+10));
        *i += 6; return (unsigned char)((hv<<4)|lv);
    }
    unsigned char b = unescape_short_btn(c); *i += 2; return b;
}

/* Read a JSON string body into out until the UNescaped closing quote. *i must
   point just past the opening quote on entry; on return it points past the
   closing quote. Returns the byte length written. */
static int read_json_string(const char *s, int *i, char *out, int cap) {
    int n = 0;
    while (s[*i] != '"' && s[*i] != 0) { unsigned char b = parse_one(s, i); if (n < cap-1) out[n++] = (char)b; }
    if (s[*i] == '"') (*i)++;
    out[n] = 0; return n;
}

/* Wrap an arbitrary byte string as the body of a JSON string (escaping each
   byte). Appends to out (which must be large enough). */
static void escape_into(const char *src, char *out) {
    char piece[8]; size_t o = strlen(out);
    for (const unsigned char *p = (const unsigned char*)src; *p; p++) {
        render_byte(*p, piece); size_t L = strlen(piece);
        memcpy(out + o, piece, L); o += L;
    }
    out[o] = 0;
}

/* Exhaustive round-trip: every byte survives render -> parse_one unchanged. */
static int roundtrip_all(void) {
    int pass = 0;
    for (int b = 0; b < 256; b++) {
        char enc[8]; render_byte((unsigned char)b, enc);
        int i = 0; unsigned char got = parse_one(enc, &i);
        if (got == (unsigned char)b && enc[i] == 0) pass++;
    }
    printf("[roundtrip] parse(render(b))==b : %d/256\n", pass);
    return pass == 256;
}

/* A value containing every JSON-hostile byte, wrapped, must still validate. */
static int adversarial_validity(void) {
    const char *hostile = "he said \"hi\"\tand \\ left\nline\x01end";
    char obj[512]; strcpy(obj, "{\"v\":\"");
    escape_into(hostile, obj);
    strcat(obj, "\"}");
    int valid = json_is_valid(obj);
    /* parse it back and confirm losslessness */
    int i = 0; while (obj[i] && obj[i] != ':') i++; i += 2; /* step to opening quote of value, then past it */
    char back[512]; read_json_string(obj, &i, back, sizeof(back));
    int lossless = (strcmp(back, hostile) == 0);
    printf("[roundtrip] adversarial value: valid=%d lossless=%d\n", valid, lossless);
    return valid && lossless;
}
```

Note on the `adversarial_validity` index walk: after `{"v":` the next char is `"` (the opening quote); `i+=2` from the `:` lands just past that opening quote, which is what `read_json_string` expects.

- [ ] **Step 2: Call both from `main` (after `build_unescape`)**

```c
    ok &= roundtrip_all();
    ok &= adversarial_validity();
```

- [ ] **Step 3: Build and run**

Run: `make jsonstory`
Expected: `[roundtrip] parse(render(b))==b : 256/256` and `[roundtrip] adversarial value: valid=1 lossless=1`; still `ALL PASS`.

- [ ] **Step 4: Checkpoint (no git)** — re-run `make jsonstory`; confirm `ALL PASS`.

---

## Task 5: TinyStories word-LM story generation

**Files:**
- Modify: `tests/jsonstory_demo.c`

- [ ] **Step 1: Add the embedded corpus, word vocab, tokenizer, and generator (above `main`)**

```c
/* ---- Task 5: word-LM story generation (cce_wordlm) ---------------------- */
/* Cased micro-corpus (same TinyStories register as endgate_demo). Cased so the
   Task-6 name-gate can label proper nouns; lowercased for LM training. */
static const char *CORPUS[] = {
    "Once upon a time there was a curious fox who found a glowing mushroom in the forest.",
    "Lily loved to play in the garden with her red ball and her happy little dog.",
    "Tom had a little black cat named Max who liked to sleep on a soft warm mat.",
    "Ben built a tall tower with colorful blocks and then laughed when it fell down.",
    "Mia the rabbit hopped fast across the sunny meadow to hide from the summer rain.",
    "Sam sailed his paper boat on the pond and the wind brought it safely back home.",
    "Emma sat by the window reading stories while the first stars appeared in the sky.",
    "Jack threw the bright red ball high into the air and his dog caught it in the yard.",
    "Sara baked sweet cookies and the warm smell made the whole house feel very happy.",
    "Anna made a wish on a shooting star and the next morning she met a brand new friend.",
    "A tiny mouse ate a crumb of cheese while the big cat was asleep on the warm mat.",
    "The young dragon practiced flying low over the green hills until he could soar high.",
    "The kind knight gave food to the hungry villagers and they thanked him with a feast.",
    "A clever bird sang beautiful songs and told the princess where the lost crown was hidden.",
    "The girl planted seeds in the spring and by summer the flowers were taller than she was.",
    "A boy found a lost puppy and after searching all day he returned it to its happy family."
};
#define NCORP ((int)(sizeof(CORPUS)/sizeof(CORPUS[0])))
#define WCTX 3
#define WVMAX 400
static char  g_word[WVMAX][24];
static int   g_nwords = 0;

static void wlower(char *w) { for (; *w; ++w) if (*w>='A'&&*w<='Z') *w=(char)(*w+32); }
static int  word_add(const char *w) {
    for (int i=0;i<g_nwords;i++) if (strcmp(g_word[i],w)==0) return i;
    if (g_nwords>=WVMAX) return -1;
    strncpy(g_word[g_nwords],w,23); g_word[g_nwords][23]=0; return g_nwords++;
}
static int  word_find(const char *w){ for(int i=0;i<g_nwords;i++) if(strcmp(g_word[i],w)==0) return i; return -1; }

/* Tokenize to lowercased word ids; '.'!'?' -> the "." token. */
static int tokenize(const char *s, int *out, int cap, int add) {
    int n=0; char cur[24]; int cl=0;
    for (const char *p=s;;++p){ char c=*p;
        int al=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='\'';
        if (al){ if(cl<23) cur[cl++]=c; }
        else { if(cl>0){cur[cl]=0; wlower(cur); int id=add?word_add(cur):word_find(cur); if(id>=0&&n<cap)out[n++]=id; cl=0;}
               if(c=='.'||c=='!'||c=='?'){int id=add?word_add("."):word_find("."); if(id>=0&&n<cap)out[n++]=id;}
               if(c==0) break; }
    }
    return n;
}

/* Train a word-LM on the corpus and free-run one story into `out`. */
static void generate_story(char *out, size_t outsz) {
    word_add(".");
    { int tmp[256]; for (int s=0;s<NCORP;s++) tokenize(CORPUS[s], tmp, 256, 1); }
    int V = g_nwords;
    cce_wordlm *m = cce_wordlm_create(V, 24, WCTX, 64, 99u);
    for (int epoch=0; epoch<200; epoch++)
        for (int s=0;s<NCORP;s++){ int t[256]; int nt=tokenize(CORPUS[s],t,256,0);
            for (int i=WCTX;i<nt;i++) cce_wordlm_train_step(m, &t[i-WCTX], t[i], 0.02f); }

    int dot = word_find("."); int ctx[WCTX];
    const char *seed[WCTX] = {"upon","a","time"};
    for (int k=0;k<WCTX;k++){ int id=word_find(seed[k]); ctx[k]=(id>=0)?id:0; }
    strncpy(out, "Once upon a time", outsz-1); out[outsz-1]=0;
    float pen[WVMAX];
    for (int step=0; step<40 && strlen(out)<outsz-32; step++) {
        for (int v=0; v<V; v++) pen[v]=0.0f;
        for (int k=0;k<WCTX;k++) pen[ctx[k]] -= 3.0f;      /* discourage immediate repeats */
        int nx = cce_wordlm_predict(m, ctx, pen);
        if (nx<0 || nx==dot) break;
        size_t L=strlen(out); snprintf(out+L, outsz-L, " %s", g_word[nx]);
        for (int k=0;k<WCTX-1;k++) ctx[k]=ctx[k+1]; ctx[WCTX-1]=nx;
    }
    size_t L=strlen(out); if (L+1<outsz){ out[L]='.'; out[L+1]=0; }
    cce_wordlm_free(m);
}
```

- [ ] **Step 2: Generate a story in `main` (after the round-trip checks) and assert it is non-trivial**

```c
    char story[2048] = {0};
    generate_story(story, sizeof(story));
    printf("[story] %s\n", story);
    int story_ok = (strlen(story) > 16) && (story[strlen(story)-1] == '.');
    ok &= story_ok;
```

- [ ] **Step 3: Build and run**

Run: `make jsonstory`
Expected: a `[story] Once upon a time ...` line of real words ending in `.`; still `ALL PASS`. (Exact wording varies; the corpus is small, so expect simple/repetitive but real-word text.)

- [ ] **Step 4: Checkpoint (no git)** — re-run `make jsonstory`; confirm `ALL PASS`.

---

## Task 6: `characters` name-gate — certified core + conformal (endgate pattern)

**Files:**
- Modify: `tests/jsonstory_demo.c`

- [ ] **Step 1: Add name labeling, the gate primitive, and character extraction (above `main`)**

```c
/* ---- Task 6: character name-gate (endgate pattern) ---------------------- */
/* A word is a NAME exemplar if it appears Capitalized mid-sentence in the cased
   corpus (i.e., not as the first token after a sentence start). Counts feed a
   per-word majority label, exactly like endgate's end/continue counts. */
static BinaryTransformNetwork g_gate;
static Contract               g_gate_c;
static ConformalCalibrator    g_gate_cal;
static int                    g_gate_V = 0;

static int build_namegate(void) {
    int V = g_nwords; g_gate_V = V;
    int *cnt_name = (int*)calloc(V,sizeof(int)), *cnt_not = (int*)calloc(V,sizeof(int));
    /* scan cased corpus: track sentence-start; a Capitalized non-start word -> name */
    for (int s=0;s<NCORP;s++){
        const char *p = CORPUS[s]; int at_start = 1; char cur[24]; int cl=0;
        for (;;++p){ char c=*p; int al=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='\'';
            if (al){ if(cl<23) cur[cl++]=c; }
            else { if(cl>0){ cur[cl]=0; char lc[24]; strcpy(lc,cur); wlower(lc); int id=word_find(lc);
                       if(id>=0){ int cap=(cur[0]>='A'&&cur[0]<='Z'); if(cap && !at_start) cnt_name[id]++; else cnt_not[id]++; }
                       at_start=0; cl=0; }
                   if(c=='.'||c=='!'||c=='?') at_start=1;
                   if(c==0) break; }
        }
    }
    /* exemplars over words seen, one-hot word -> {not,name}; majority label. */
    int seen=0; for (int w=0;w<V;w++) if (cnt_name[w]+cnt_not[w]>0) seen++;
    double *X=(double*)calloc((size_t)seen*V,sizeof(double));
    double *Y=(double*)calloc((size_t)seen*2,sizeof(double));
    size_t *kT=(size_t*)calloc((size_t)seen,sizeof(size_t));
    double *kX=(double*)calloc((size_t)seen*V,sizeof(double));
    int r=0;
    for (int w=0;w<V;w++){ if(cnt_name[w]+cnt_not[w]==0) continue;
        int name = cnt_name[w] > cnt_not[w] ? 1 : 0;
        X[(size_t)r*V+w]=1.0; Y[(size_t)r*2+name]=1.0;
        kX[(size_t)r*V+w]=1.0; kT[r]=(size_t)name; r++; }

    if (btn_init(&g_gate, (size_t)V, 2, 16, 64, 0.05, 2468u)!=0){ printf("[gate] btn_init failed\n"); return 0; }
    Port pin={PORT_ONEHOT,(size_t)V,1,""}; port_set_tag(&pin,"story_word");
    Port pout={PORT_ONEHOT,2,1,""};        port_set_tag(&pout,"is_name");
    btn_set_ports(&g_gate,pin,pout);
    btn_train(&g_gate, X, Y, (size_t)seen, 400);

    if (contract_init_borrowed(&g_gate_c,"character_name_gate",&g_gate,X,Y,(size_t)seen)!=0){
        printf("[gate] contract_init_borrowed failed\n"); return 0; }
    CertifyReport rep; memset(&rep,0,sizeof(rep));
    int cert = btn_certify(&g_gate,&g_gate_c,&rep);

    double alpha=0.10;
    int calib_ok = (conformal_calibrate_btn(&g_gate_cal,&g_gate,&g_gate_c,kX,kT,(size_t)seen,alpha)==0);
    printf("[gate] character_name_gate: %s (%zu/%zu exemplars), conformal coverage=%.2f\n",
           cert==0?"CERTIFIED":"partial", rep.passed, rep.exemplars,
           calib_ok?conformal_coverage_level(&g_gate_cal):0.0);
    /* tables borrowed by contract/calibrator: keep alive (demo leaks). */
    return (cert==0) && calib_ok;
}

/* Extract distinct character names from a story: words the gate accepts (==1).
   Conformal ABSTAIN (-1) or reject (0) are dropped. Returns count. */
static int extract_characters(const char *story, char names[][24], int cap) {
    int toks[256]; int nt=tokenize(story,toks,256,0); int nc=0;
    for (int i=0;i<nt && nc<cap;i++){ int w=toks[i]; if(w<0) continue;
        double *in=(double*)calloc((size_t)g_gate_V,sizeof(double)); in[w]=1.0;
        int dec=conformal_classify_or_abstain(&g_gate_cal,&g_gate,&g_gate_c,in); free(in);
        if (dec==1){ int dup=0; for(int j=0;j<nc;j++) if(strcmp(names[j],g_word[w])==0) dup=1;
            if(!dup){ strncpy(names[nc],g_word[w],23); names[nc][23]=0; nc++; } }
    }
    return nc;
}
```

- [ ] **Step 2: Call from `main` (after the story is generated)**

```c
    ok &= build_namegate();
    char names[16][24]; int nnames = extract_characters(story, names, 16);
    printf("[characters] %d found:", nnames);
    for (int i=0;i<nnames;i++) printf(" %s", names[i]);
    printf("\n");
```

- [ ] **Step 3: Build and run**

Run: `make jsonstory`
Expected: `[gate] character_name_gate: CERTIFIED (N/N exemplars), conformal coverage=0.90` and a `[characters] ...` line. The list may be empty if the generated story contains no proper nouns — that is acceptable (the gate's job is correctness, not recall); still `ALL PASS`.

- [ ] **Step 4: Checkpoint (no git)** — re-run `make jsonstory`; confirm `ALL PASS`.

---

## Task 7: Assemble JSON, validate, parse back, final gate

**Files:**
- Modify: `tests/jsonstory_demo.c`

- [ ] **Step 1: Add the title rule, the assembler, and the parse-back checker (above `main`)**

```c
/* ---- Task 7: assemble {title,characters,story}, validate, parse back ---- */
/* Deterministic title: Title-Case the first content word after the seed/stop
   set. Its bytes are escaped by the PROVEN escaper, so the title is JSON-safe
   without a separate certificate. */
static void make_title(const char *story, char *title, size_t cap) {
    const char *stop[5] = {"once","upon","a","an","the"};
    int toks[256]; int nt=tokenize(story,toks,256,0);
    const char *pick = NULL;
    for (int i=0;i<nt;i++){ int w=toks[i]; if(w<0) continue; const char *ww=g_word[w];
        if (strcmp(ww,".")==0) continue; int isstop=0; for(int k=0;k<5;k++) if(strcmp(ww,stop[k])==0) isstop=1;
        if (!isstop){ pick=ww; break; } }
    if (!pick) pick = (nt>0 && toks[0]>=0) ? g_word[toks[0]] : "story";
    snprintf(title, cap, "%s", pick);
    if (title[0]>='a'&&title[0]<='z') title[0]=(char)(title[0]-32);  /* Title-Case */
}

/* Build {"title":"..","characters":["..",..],"story":".."} with escaped values. */
static void assemble_json(const char *title, char names[][24], int nnames,
                          const char *story, char *out, size_t outsz) {
    (void)outsz;
    strcpy(out, "{\"title\":\""); escape_into(title, out); strcat(out, "\",\"characters\":[");
    for (int i=0;i<nnames;i++){ if(i) strcat(out,","); strcat(out,"\""); escape_into(names[i],out); strcat(out,"\""); }
    strcat(out, "],\"story\":\""); escape_into(story, out); strcat(out, "\"}");
}

/* Parse our fixed frame back into fields. Returns 1 on a clean structural match. */
static int parse_back(const char *s, char *title, char *story,
                      char names[][24], int *nnames, int cap) {
    int i=0;
    if (strncmp(s+i,"{\"title\":\"",10)!=0) return 0; i+=10;
    read_json_string(s,&i,title,256);
    if (strncmp(s+i,",\"characters\":[",15)!=0) return 0; i+=15;
    int nc=0;
    if (s[i]!=']'){ for(;;){ if(s[i]!='"') return 0; i++; read_json_string(s,&i,names[nc<cap?nc:0],24); if(nc<cap) nc++;
                       if(s[i]==',' ){ i++; continue; } if(s[i]==']'){ break; } return 0; } }
    if (s[i]!=']') return 0; i++; *nnames=nc;
    if (strncmp(s+i,",\"story\":\"",10)!=0) return 0; i+=10;
    read_json_string(s,&i,story,2048);
    if (strncmp(s+i,"}",1)!=0) return 0;
    return 1;
}

static int assemble_and_check(const char *story, char names[][24], int nnames) {
    char title[256]; make_title(story, title, sizeof(title));
    char obj[4096]; assemble_json(title, names, nnames, story, obj, sizeof(obj));
    int valid = json_is_valid(obj);

    char t2[256], s2[2048], n2[16][24]; int nn2=0;
    int parsed = parse_back(obj, t2, s2, n2, &nn2, 16);
    int fields_ok = parsed && strcmp(t2,title)==0 && strcmp(s2,story)==0 && nn2==nnames;
    for (int i=0;i<nnames && fields_ok;i++) if (strcmp(n2[i],names[i])!=0) fields_ok=0;

    printf("\n[json] %s\n", obj);
    printf("[json] json_is_valid=%d  parse_back_fields_ok=%d\n", valid, fields_ok);
    return valid && fields_ok;
}
```

- [ ] **Step 2: Call from `main` (after characters are extracted)**

```c
    ok &= assemble_and_check(story, names, nnames);
```

- [ ] **Step 3: Build and run**

Run: `make jsonstory`
Expected: a `[json] {"title":"...","characters":[...],"story":"Once upon a time ..."}` line, then `[json] json_is_valid=1  parse_back_fields_ok=1`, then `ALL PASS`, exit 0.

- [ ] **Step 4: Final verification — full acceptance check**

Run: `make jsonstory; echo "exit=$?"`
Confirm ALL of these lines are present and the exit is 0:
- `[escape] json_escape_class: CERT_PROVEN`
- `[unescape] json_unescape_short: CERT_PROVEN`
- `[roundtrip] parse(render(b))==b : 256/256`
- `[roundtrip] adversarial value: valid=1 lossless=1`
- `[gate] character_name_gate: CERTIFIED ...`
- `[json] ... "story":"Once upon a time ..."`
- `[json] json_is_valid=1  parse_back_fields_ok=1`
- `ALL PASS`, `exit=0`

This is the acceptance criteria: a trained JSON capability backed by a PROVEN contract, grounded in TinyStories (name-gate + story LM), generating JSON that contains a story and parses back losslessly.

- [ ] **Step 5: Checkpoint (no git)** — re-run `make jsonstory`; confirm `ALL PASS`.

---

## Self-Review (against the spec)

**Spec coverage:**
- §4.1 `json_escape_class` PROVEN → Task 2. ✓
- §4.2 `json_unescape` PROVEN (understand side) → Task 3 (`json_unescape_short`). ✓
- §4.3 round-trip losslessness (256/256, exhaustive replay) → Task 4. ✓
- §4.4 story slot from the word-LM → Task 5 (`cce_wordlm`). ✓
- §4.5 title derived deterministically → Task 7 `make_title`. **Deviation:** the per-byte Title-Case map is **not** registered as a separate certified primitive (gold-plating); the title's bytes pass through the PROVEN escaper, so JSON-safety is already guaranteed. Spec §4.5 updated to match.
- §4.6 character name-gate (certify + conformal) → Task 6. ✓
- §5 generate/understand flows → Tasks 5–7. ✓
- §6 single demo + `make jsonstory` + printed verdicts/round-trip/conformal/JSON/parse-back → Tasks 1–7. ✓
- §7 TDD (assertions first, then implement) → every task adds the assertion in the same step it adds the code, and Step "Build and run" verifies; reuse `json_is_valid` via `tests/json_validate.h` → Task 1. ✓
- §8 out-of-scope respected (no streaming LM, fixed schema, no persisted certs, per-word gate not full NER). ✓

**Placeholder scan:** No TBD/TODO; every code step shows complete, compiling code; the one extraction step (Task 1 Step 1) gives exact source line ranges to copy. ✓

**Type consistency:** `escape_class_btn`/`ref_escape_class` share the `ESC_*` enum; `render_byte` switches on the same classes; `parse_one` ↔ `render_byte` are inverse; `unescape_short_btn` uses the `ESC_LETTERS`/`ESC_LETTER_BYTE` arrays consistently; `g_gate`/`g_gate_c`/`g_gate_cal`/`g_gate_V` names match across Task 6 and `extract_characters`; `assemble_json`/`parse_back` use the identical fixed frame literals. ✓

---

## Execution note

Recommended: inline execution via `superpowers:executing-plans` (the tasks form one growing file; batching with a build/run checkpoint per task fits well). Subagent-driven is also fine — dispatch one subagent per task, reviewing the build output between tasks.

---

## Implementation notes (deviations during execution — 2026-06-26)

Executed inline; all 7 tasks landed and the final `make jsonstory` is green (exit 0).
The plan's structure held; the tuning below is the delta from the literal code blocks.
**Root theme: BTN *exact* certification needs class balance** — rare output classes
get drowned by common ones and fail `btn_certify`/exhaustive sweep. Fix everywhere:
oversample the rare cases in the **training** table while certifying on the **clean
canonical exemplar table** (keeps the proof honest).

- **Task 2 (`json_escape_class`):** first attempt NOT PROVEN — the 7 singleton escape
  classes (`"`,`\`,`\b`,`\t`,`\n`,`\f`,`\r`) were swamped by PASS (~92) + UNI (~158).
  Fix: oversample those 7 bytes (`REP=40`), capacity `64/512`, `1500` epochs → PROVEN.
- **Task 3 (`json_unescape_short`):** first attempt NOT PROVEN — a 256-wide output from
  only 7 examples was near-flat (min-margin 0.011), all 7 read ambiguous. Fix: capacity
  `64/256`, lr `0.08`, `8000` epochs → PROVEN, min-margin 0.965.
- **Task 6 (`character_name_gate`):** the planned "capitalized mid-sentence" heuristic is
  wrong — sentence-initial proper nouns (`Tom`,`Lily`,…) are mislabeled *not*-name, leaving
  `max` ("named Max") as a lone positive that the imbalance-biased net collapsed
  (171/172). Fix: label from the corpus **character lexicon** (`NAMES[]`), oversample the
  ~10 positives (`REP=16`), capacity `64/256`, `1500` epochs → CERTIFIED 172/172, conformal
  coverage 0.90. Added a `gate_probe()` (lily/tom/max=NAME, the/fox/forest=not).
- **Task 5/7 (story + JSON):** split `generate_story` into `train_lm()` (once) +
  `freerun_story(seed,prefix,…)`. Emit **two** JSON objects to exercise the schema both
  ways: canonical `"Once upon a time …"` (characters `[]`, honest — no proper nouns) and
  `"Tom had a …"` (characters `["tom","max"]` populated). Both `json_is_valid=1` and
  parse back losslessly.

Note: changes are purely additive (new `tests/jsonstory_demo.c`, new `tests/json_validate.h`,
new `make jsonstory` target). No existing source modified, so other targets/tests are
unaffected.
