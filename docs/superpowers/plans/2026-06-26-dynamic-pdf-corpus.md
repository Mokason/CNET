# Dynamic PDF Corpus → Replay-Trained Word-LM — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give CNET a pure-C pipeline that reads a text-born PDF, autosplits it into a persistent append-only corpus that grows across ingests (uncapped), and replay-trains `cce_wordlm` on the full corpus to free-run real-word text.

**Architecture:** Four self-contained modules — `pdf/inflate` (DEFLATE), `pdf/pdf_extract` (text-born extractor + space heuristic + detect-and-skip), `corpus/corpus_split` (text→clean sentences), `corpus/corpus_store` (persistent append-only, dedup) — plus a `pdflearn` driver that ingests PDFs, appends to the store, and replay-trains `cce_wordlm`. Heuristic parsing gets **no contracts**; this is data infrastructure feeding the LM. Zero edits to core/router/contract/CCE sources.

**Tech Stack:** C11 (gcc, `-mno-avx` per repo CFLAGS), stdlib + libm only, `src/cce/cce_wordlm.c` (training). Tests use **embedded** byte vectors / PDFs (no network/external files); compressed test vectors generated offline with PowerShell `System.IO.Compression.DeflateStream`.

**Project note — NO GIT:** This repo's `.git` was removed. Run **no** git commands. "Commit" steps are replaced by a **checkpoint**: re-run the relevant `make` target and confirm assertions still pass.

---

## File Structure

- **Create `include/pdf/inflate.h`, `src/pdf/inflate.c`** — raw-DEFLATE inflater (`puff`) + `pdf_flate_decode()` (zlib-header strip).
- **Create `include/pdf/pdf_extract.h`, `src/pdf/pdf_extract.c`** — `pdf_extract_text()` + `PdfStatus`.
- **Create `include/corpus/corpus_split.h`, `src/corpus/corpus_split.c`** — `StrList` + `corpus_split()`.
- **Create `include/corpus/corpus_store.h`, `src/corpus/corpus_store.c`** — `corpus_append/load/count`.
- **Create `tests/test_pdf.c`** — self-checking unit tests for all four modules (`make pdftest`).
- **Create `tests/pdflearn_demo.c`** — the driver (`make pdflearn [file.pdf ...]`); growable vocab + replay training + end-to-end self-checks.
- **Modify `Makefile`** — vars, `pdftest` + `pdflearn` targets, `.PHONY`, `clean`.

Shared convention for `tests/test_pdf.c`: a `CHECK` macro that counts failures and a `main` returning nonzero if any failed.

```c
static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg) do { g_checks++; if(!(cond)){ g_fail++; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);} } while(0)
```

---

## Task 1: `inflate` — raw DEFLATE + FlateDecode wrapper

**Files:**
- Create: `include/pdf/inflate.h`, `src/pdf/inflate.c`, `tests/test_pdf.c`
- Modify: `Makefile`

**Note on scope:** the inflater is the one large *standard* algorithm (RFC 1951). It is specified by its interface + a **deterministic gating test**; the body is the textbook DEFLATE decoder (bit reader LSB-first; stored / fixed-Huffman / dynamic-Huffman blocks; canonical Huffman from code lengths; LZ77 length/dist back-references with the standard base+extra-bit tables). Write it, then make the gating test pass — if it fails, debug against the vector (it is exact).

- [ ] **Step 1: Header**

`include/pdf/inflate.h`:
```c
#ifndef PDF_INFLATE_H
#define PDF_INFLATE_H
#include <stddef.h>
/* Raw DEFLATE (RFC 1951), no zlib header. dest/destlen in/out (cap/used). 0=ok, <0=error. */
int  puff(unsigned char *dest, unsigned long *destlen,
          const unsigned char *source, unsigned long *sourcelen);
/* FlateDecode: skip the 2-byte zlib header (0x78 ..), inflate, ignore trailing adler32.
   Returns bytes written into out, or -1 on malformed input / overflow. */
long pdf_flate_decode(const unsigned char *in, size_t in_len,
                      unsigned char *out, size_t out_cap);
#endif
```

- [ ] **Step 2: Generate the DEFLATE test vector (offline) and capture its bytes**

Run (PowerShell):
```powershell
$txt = "the quick brown fox jumps over the lazy dog. " * 4
$s = [Text.Encoding]::ASCII.GetBytes($txt)
$ms = New-Object IO.MemoryStream
$ds = New-Object IO.Compression.DeflateStream($ms,[IO.Compression.CompressionMode]::Compress)
$ds.Write($s,0,$s.Length); $ds.Close()
($ms.ToArray() | ForEach-Object { '0x{0:x2}' -f $_ }) -join ','
```
This prints the RAW DEFLATE bytes of the known plaintext. Keep both the plaintext string and the byte list for the test.

- [ ] **Step 3: Write the failing test in `tests/test_pdf.c`**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/pdf/inflate.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg) do { g_checks++; if(!(cond)){ g_fail++; printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);} } while(0)

static void test_inflate(void) {
    /* <-- paste the byte list from Step 2 here --> */
    static const unsigned char VEC[] = { /* 0x..,0x.. */ };
    const char *want = "the quick brown fox jumps over the lazy dog. "
                       "the quick brown fox jumps over the lazy dog. "
                       "the quick brown fox jumps over the lazy dog. "
                       "the quick brown fox jumps over the lazy dog. ";
    unsigned char out[1024]; unsigned long olen = sizeof(out), slen = sizeof(VEC);
    int rc = puff(out, &olen, VEC, &slen);
    CHECK(rc == 0, "puff returns 0");
    CHECK(olen == strlen(want), "puff output length matches");
    CHECK(olen <= sizeof(out) && memcmp(out, want, olen) == 0, "puff output bytes match");
}

int main(void) {
    printf("=== test_pdf ===\n");
    test_inflate();
    printf("%d/%d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
```

- [ ] **Step 4: Add the Makefile pieces and verify the test FAILS to link**

In `Makefile` near other source vars:
```make
PDF_SRC := src/pdf/inflate.c src/pdf/pdf_extract.c
CORPUS_SRC := src/corpus/corpus_split.c src/corpus/corpus_store.c
PDF_TEST := tests/test_pdf.c
PDFLEARN_DEMO := tests/pdflearn_demo.c
```
(For Task 1 only, temporarily build with just `src/pdf/inflate.c` so the missing later files don't block — use the literal in the target. Restore `$(PDF_SRC)` in Task 2 once `pdf_extract.c` exists.) Add target:
```make
pdftest: src/pdf/inflate.c $(PDF_TEST) include/pdf/inflate.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ src/pdf/inflate.c $(PDF_TEST) $(LDFLAGS)
	./pdftest
```
Add `pdftest pdflearn` to `.PHONY` (line 117) and `pdftest pdflearn` to the `clean` rm list (~line 570).

Run: `make pdftest`
Expected: link error (`undefined reference to 'puff'`) — confirms the test is wired and red.

- [ ] **Step 5: Implement `src/pdf/inflate.c`**

Write the raw-DEFLATE decoder behind `puff()` (textbook RFC 1951: LSB-first bit reader; BTYPE 0 stored / 1 fixed / 2 dynamic; canonical Huffman built from code-length counts; length codes 257–285 and distance codes 0–29 with the standard base+extra tables; copy back-references into `dest`, bounded by `*destlen`). Then:
```c
long pdf_flate_decode(const unsigned char *in, size_t in_len,
                      unsigned char *out, size_t out_cap) {
    if (in_len < 2) return -1;
    /* zlib stream: 1 byte CMF, 1 byte FLG, then raw DEFLATE, then 4-byte adler32.
       puff stops at the final block, so the adler tail is simply not consumed. */
    const unsigned char *src = in + 2; size_t srclen = in_len - 2;
    unsigned long olen = out_cap, slen = srclen;
    if (puff(out, &olen, src, &slen) != 0) return -1;
    return (long)olen;
}
```

- [ ] **Step 6: Run the test to PASS**

Run: `make pdftest`
Expected: `test_inflate` checks pass; `N/N checks passed`; exit 0. If output mismatches, the decoder has a bug — debug against `VEC` (deterministic).

- [ ] **Step 7: Checkpoint (no git)** — re-run `make pdftest`; confirm green.

---

## Task 2: `pdf_extract` — uncompressed streams, `Tj` + string literals

**Files:**
- Create: `include/pdf/pdf_extract.h`, `src/pdf/pdf_extract.c`
- Modify: `tests/test_pdf.c`, `Makefile` (restore `$(PDF_SRC)` in the `pdftest` target)

- [ ] **Step 1: Header**

`include/pdf/pdf_extract.h`:
```c
#ifndef PDF_EXTRACT_H
#define PDF_EXTRACT_H
#include <stddef.h>
typedef enum { PDF_OK=0, PDF_ENCRYPTED, PDF_UNSUPPORTED_FONTS, PDF_NO_TEXT, PDF_MALFORMED } PdfStatus;
/* Extract text-born content into out (nul-terminated, truncated to cap). *out_len gets
   the byte count written. Never reads past pdf[n]. */
PdfStatus pdf_extract_text(const unsigned char *pdf, size_t n,
                           char *out, size_t cap, size_t *out_len);
#endif
```

- [ ] **Step 2: Failing test — uncompressed PDF with `(Hello World) Tj`**

Add to `tests/test_pdf.c` (and call from `main`):
```c
#include "../include/pdf/pdf_extract.h"

/* Minimal uncompressed PDF: one content stream "BT (Hello World) Tj ET". */
static const char PDF_PLAIN[] =
"%PDF-1.4\n"
"1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
"2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
"3 0 obj<</Type/Page/Parent 2 0 R/Contents 4 0 R>>endobj\n"
"4 0 obj<</Length 24>>stream\n"
"BT (Hello World) Tj ET\n"
"endstream endobj\n"
"trailer<</Root 1 0 R>>\n";

static void test_extract_plain(void) {
    char out[256]; size_t olen = 0;
    PdfStatus st = pdf_extract_text((const unsigned char*)PDF_PLAIN, sizeof(PDF_PLAIN)-1, out, sizeof(out), &olen);
    CHECK(st == PDF_OK, "plain PDF status OK");
    CHECK(strstr(out, "Hello World") != NULL, "plain PDF yields 'Hello World'");
}
```
Run: `make pdftest` → link error (`pdf_extract_text` undefined). (Update the `pdftest` target to link `$(PDF_SRC)` now: `pdftest: $(PDF_SRC) $(PDF_TEST) include/pdf/inflate.h include/pdf/pdf_extract.h` and the recipe to `$(PDF_SRC) $(PDF_TEST)`.)

- [ ] **Step 3: Implement the uncompressed path in `src/pdf/pdf_extract.c`**

```c
#include "../../include/pdf/pdf_extract.h"
#include "../../include/pdf/inflate.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* append one byte to out (bounded). */
static void emit(char *out, size_t cap, size_t *o, char c) { if (*o + 1 < cap) out[(*o)++] = c; }

/* Decode a (...) literal starting at s[i]=='(' ; append text; advance i past ')'. */
static void decode_literal(const unsigned char *s, size_t n, size_t *i, char *out, size_t cap, size_t *o) {
    (*i)++; int depth = 1;
    while (*i < n && depth > 0) {
        unsigned char c = s[*i];
        if (c == '\\') {
            (*i)++; if (*i >= n) break; unsigned char e = s[*i];
            if (e=='n') emit(out,cap,o,'\n'); else if (e=='r') emit(out,cap,o,'\r');
            else if (e=='t') emit(out,cap,o,'\t'); else if (e=='(') emit(out,cap,o,'(');
            else if (e==')') emit(out,cap,o,')'); else if (e=='\\') emit(out,cap,o,'\\');
            else if (e>='0'&&e<='7') { int v=0,k=0; while(k<3 && *i<n && s[*i]>='0'&&s[*i]<='7'){v=v*8+(s[*i]-'0');(*i)++;k++;} (*i)--; emit(out,cap,o,(char)v); }
            else emit(out,cap,o,(char)e);
            (*i)++;
        } else if (c=='(') { depth++; emit(out,cap,o,'('); (*i)++; }
        else if (c==')') { depth--; if (depth>0) emit(out,cap,o,')'); (*i)++; }
        else { emit(out,cap,o,(char)c); (*i)++; }
    }
}

/* Process a decoded content stream: pull text from (...) Tj/TJ. (TJ + hex added later.) */
static void process_content(const unsigned char *s, size_t n, char *out, size_t cap, size_t *o) {
    for (size_t i = 0; i < n; ) {
        if (s[i]=='(') { decode_literal(s,n,&i,out,cap,o); emit(out,cap,o,' '); }
        else i++;
    }
}

PdfStatus pdf_extract_text(const unsigned char *pdf, size_t n, char *out, size_t cap, size_t *out_len) {
    size_t o = 0; out[0]=0;
    /* find each "stream\n"..."endstream"; uncompressed content only for now. */
    const char *p = (const char*)pdf;
    for (size_t i = 0; i + 6 < n; ) {
        if (memcmp(p+i, "stream", 6) == 0) {
            size_t s0 = i + 6;
            if (s0 < n && p[s0]=='\r') s0++;
            if (s0 < n && p[s0]=='\n') s0++;
            /* find endstream */
            size_t e = s0;
            while (e + 9 < n && memcmp(p+e, "endstream", 9) != 0) e++;
            process_content(pdf + s0, e - s0, out, cap, &o);
            i = e + 9;
        } else i++;
    }
    out[o] = 0; if (out_len) *out_len = o;
    return o > 0 ? PDF_OK : PDF_NO_TEXT;
}
```

- [ ] **Step 4: Run → PASS**

Run: `make pdftest`
Expected: `test_extract_plain` passes; all green; exit 0.

- [ ] **Step 5: Checkpoint (no git)** — re-run `make pdftest`.

---

## Task 3: `pdf_extract` — `TJ` arrays + space heuristic

**Files:**
- Modify: `src/pdf/pdf_extract.c`, `tests/test_pdf.c`

- [ ] **Step 1: Failing test — `TJ` with kerning inserts a space**

```c
static const char PDF_TJ[] =
"%PDF-1.4\n4 0 obj<</Length 40>>stream\n"
"BT [(Hello)-400(World)] TJ ET\n"
"endstream endobj\n";

static void test_extract_tj(void) {
    char out[256]; size_t olen=0;
    PdfStatus st = pdf_extract_text((const unsigned char*)PDF_TJ, sizeof(PDF_TJ)-1, out, sizeof(out), &olen);
    CHECK(st == PDF_OK, "TJ PDF status OK");
    CHECK(strstr(out, "Hello World") != NULL || strstr(out, "Hello  World") != NULL, "TJ inserts space between Hello and World");
}
```
Run `make pdftest` → fails (current `process_content` ignores the `-400` kern, yields "HelloWorld").

- [ ] **Step 2: Implement TJ handling in `process_content`**

Replace `process_content` with a version that, when inside `[ ... ] TJ`, decodes each `(...)` and inserts a space when a numeric kern token between strings is `< -SPACE_KERN` (negative kerning = visible gap). Outside brackets, `(...) Tj` works as before.
```c
#define SPACE_KERN 120.0   /* tuned: |kern| above this (text units) => a word gap */

static void process_content(const unsigned char *s, size_t n, char *out, size_t cap, size_t *o) {
    for (size_t i = 0; i < n; ) {
        if (s[i]=='[') {            /* TJ array: strings + kern numbers */
            i++;
            while (i < n && s[i] != ']') {
                if (s[i]=='(') { decode_literal(s,n,&i,out,cap,o); }
                else if (s[i]=='-' || (s[i]>='0'&&s[i]<='9') || s[i]=='.') {
                    double v = strtod((const char*)s+i, NULL);
                    /* advance past the number */
                    if (s[i]=='-'||s[i]=='+') i++;
                    while (i<n && ((s[i]>='0'&&s[i]<='9')||s[i]=='.')) i++;
                    if (v < -SPACE_KERN) emit(out,cap,o,' ');
                } else i++;
            }
            if (i<n && s[i]==']') i++;
            emit(out,cap,o,' ');     /* end of the TJ run is a token boundary */
        }
        else if (s[i]=='(') { decode_literal(s,n,&i,out,cap,o); emit(out,cap,o,' '); }
        else i++;
    }
}
```

- [ ] **Step 3: Run → PASS**

Run: `make pdftest`
Expected: `test_extract_tj` + `test_extract_plain` pass.

- [ ] **Step 4: Checkpoint (no git)** — re-run `make pdftest`.

---

## Task 4: `pdf_extract` — FlateDecode streams, hex strings, encrypted-skip

**Files:**
- Modify: `src/pdf/pdf_extract.c`, `tests/test_pdf.c`

- [ ] **Step 1: Generate a FlateDecode content vector (offline)**

Run (PowerShell) to get the RAW DEFLATE of the content `BT (Hello Flate) Tj ET`:
```powershell
$s = [Text.Encoding]::ASCII.GetBytes("BT (Hello Flate) Tj ET")
$ms = New-Object IO.MemoryStream
$ds = New-Object IO.Compression.DeflateStream($ms,[IO.Compression.CompressionMode]::Compress)
$ds.Write($s,0,$s.Length); $ds.Close()
($ms.ToArray() | ForEach-Object { '0x{0:x2}' -f $_ }) -join ','
```
The FlateDecode stream bytes are `0x78,0x9c,` followed by this list (zlib header prepended; the adler tail is unnecessary because `puff` stops at the final block).

- [ ] **Step 2: Failing test — embed a FlateDecode PDF; and an `/Encrypt` PDF**

```c
/* content stream bytes = 0x78,0x9c + raw DEFLATE of "BT (Hello Flate) Tj ET" */
static const unsigned char FLATE_STREAM[] = { 0x78,0x9c, /* <-- paste Step 1 bytes --> */ };

static void test_extract_flate(void) {
    /* Build a PDF in a buffer: header + obj dict with /FlateDecode + stream<bytes>endstream. */
    unsigned char pdf[512]; size_t k=0;
    const char *head = "%PDF-1.4\n4 0 obj<</Filter/FlateDecode/Length 99>>stream\n";
    memcpy(pdf+k, head, strlen(head)); k+=strlen(head);
    memcpy(pdf+k, FLATE_STREAM, sizeof(FLATE_STREAM)); k+=sizeof(FLATE_STREAM);
    const char *tail = "\nendstream endobj\n";
    memcpy(pdf+k, tail, strlen(tail)); k+=strlen(tail);
    char out[256]; size_t olen=0;
    PdfStatus st = pdf_extract_text(pdf, k, out, sizeof(out), &olen);
    CHECK(st == PDF_OK, "flate PDF status OK");
    CHECK(strstr(out, "Hello Flate") != NULL, "flate PDF yields 'Hello Flate'");
}

static void test_extract_encrypted(void) {
    const char *enc = "%PDF-1.4\ntrailer<</Root 1 0 R/Encrypt 5 0 R>>\n";
    char out[64]; size_t olen=0;
    PdfStatus st = pdf_extract_text((const unsigned char*)enc, strlen(enc), out, sizeof(out), &olen);
    CHECK(st == PDF_ENCRYPTED, "encrypted PDF is detected and skipped");
}
```
Run `make pdftest` → fails (flate not decoded; encrypt not detected).

- [ ] **Step 3: Implement FlateDecode + `<hex>` strings + `/Encrypt` detection**

In `pdf_extract_text`, before the stream loop, scan for `/Encrypt` and bail:
```c
    if (n >= 8) {
        for (size_t i = 0; i + 8 < n; i++)
            if (memcmp(p+i, "/Encrypt", 8) == 0) { if(out_len)*out_len=0; out[0]=0; return PDF_ENCRYPTED; }
    }
```
When a stream is found, look back a bounded window (e.g. 200 bytes before `stream`) for `/FlateDecode`; if present, inflate into a scratch buffer and run `process_content` on the decoded bytes; else run it on the raw bytes:
```c
    int is_flate = 0;
    { size_t lo = (i > 200) ? i - 200 : 0;
      for (size_t j = lo; j + 12 < i; j++) if (memcmp(p+j,"/FlateDecode",12)==0){ is_flate=1; break; } }
    if (is_flate) {
        static unsigned char dec[1<<20];
        long dl = pdf_flate_decode(pdf + s0, e - s0, dec, sizeof(dec));
        if (dl > 0) process_content(dec, (size_t)dl, out, cap, &o);
    } else {
        process_content(pdf + s0, e - s0, out, cap, &o);
    }
```
Add `<hex>` string support in `process_content` (a `<...>` run → pairs of hex digits → bytes, treated like a literal):
```c
        else if (s[i]=='<' && i+1<n && s[i+1] != '<') {   /* hex string (not a << dict) */
            i++; int hi=-1;
            while (i<n && s[i]!='>') {
                int d = isxdigit(s[i]) ? (s[i]<='9'?s[i]-'0':(tolower(s[i])-'a'+10)) : -1;
                if (d>=0){ if(hi<0) hi=d; else { emit(out,cap,o,(char)((hi<<4)|d)); hi=-1; } }
                i++;
            }
            if (i<n && s[i]=='>') i++;
            emit(out,cap,o,' ');
        }
```

- [ ] **Step 4: Run → PASS**

Run: `make pdftest`
Expected: `test_extract_flate`, `test_extract_encrypted`, plus prior extract tests pass.

- [ ] **Step 5: Checkpoint (no git)** — re-run `make pdftest`.

---

## Task 5: `corpus_split` — text → clean sentences

**Files:**
- Create: `include/corpus/corpus_split.h`, `src/corpus/corpus_split.c`
- Modify: `tests/test_pdf.c`, `Makefile` (`pdftest` target links `$(CORPUS_SRC)` too)

- [ ] **Step 1: Header (defines the shared `StrList`)**

`include/corpus/corpus_split.h`:
```c
#ifndef CORPUS_SPLIT_H
#define CORPUS_SPLIT_H
#include <stddef.h>
typedef struct { char **lines; size_t count, cap; } StrList;  /* realloc-grown */
void strlist_init(StrList *s);
void strlist_push(StrList *s, const char *line);   /* copies line */
void strlist_free(StrList *s);
/* text -> cleaned sentences appended to out. Pure, no I/O. */
void corpus_split(const char *text, StrList *out);
#endif
```

- [ ] **Step 2: Failing tests**

```c
#include "../include/corpus/corpus_split.h"

static void test_split(void) {
    StrList s; strlist_init(&s);
    corpus_split("The cat sat. It was warm! Was it?  A bit.", &s);
    CHECK(s.count == 4, "splits into 4 sentences");
    CHECK(s.count>0 && strcmp(s.lines[0], "The cat sat.")==0, "first sentence clean");
    strlist_free(&s);

    StrList h; strlist_init(&h);
    corpus_split("a long exam-\nple word here that is fine.", &h);
    CHECK(h.count==1 && strstr(h.lines[0],"example")!=NULL, "de-hyphenates line breaks");
    strlist_free(&h);

    StrList j; strlist_init(&j);
    corpus_split("ok.\n  42\n  also good enough here.", &j);
    int has42=0; for(size_t k=0;k<j.count;k++) if(strcmp(j.lines[k],"42")==0) has42=1;
    CHECK(!has42, "bare page-number line dropped");
    strlist_free(&j);
}
```
Run `make pdftest` → link error (`corpus_split` undefined). Update the `pdftest` target to include `$(CORPUS_SRC)` and the corpus headers.

- [ ] **Step 3: Implement `src/corpus/corpus_split.c`**

```c
#include "../../include/corpus/corpus_split.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void strlist_init(StrList *s){ s->lines=NULL; s->count=0; s->cap=0; }
void strlist_push(StrList *s, const char *line){
    if (s->count==s->cap){ s->cap = s->cap? s->cap*2 : 16; s->lines=(char**)realloc(s->lines, s->cap*sizeof(char*)); }
    s->lines[s->count++] = strdup(line);
}
void strlist_free(StrList *s){ for(size_t i=0;i<s->count;i++) free(s->lines[i]); free(s->lines); strlist_init(s); }

/* keep alnum + basic punctuation; collapse runs of space; trim. */
static int is_pagenumber(const char *t){ for(const char*p=t;*p;p++) if(!isdigit((unsigned char)*p)) return 0; return *t!=0; }

void corpus_split(const char *text, StrList *out) {
    /* 1) de-hyphenate "x-\n y" -> "xy"; normalize all whitespace to single spaces. */
    size_t n = strlen(text);
    char *buf = (char*)malloc(n+1); size_t b=0;
    for (size_t i=0;i<n;i++){
        char c = text[i];
        if (c=='-' && i+1<n && (text[i+1]=='\n'||text[i+1]=='\r')) { /* skip hyphen + newline + following spaces */
            i++; while(i+1<n && (text[i+1]=='\n'||text[i+1]=='\r'||text[i+1]==' '||text[i+1]=='\t')) i++;
            continue;
        }
        if (c=='\n'||c=='\r'||c=='\t') c=' ';
        if (c==' ' && b>0 && buf[b-1]==' ') continue;
        buf[b++]=c;
    }
    buf[b]=0;
    /* 2) split on . ! ? ; trim; drop junk (len<6 or bare page number). */
    size_t start=0;
    for (size_t i=0;i<=b;i++){
        char c = buf[i];
        if (c=='.'||c=='!'||c=='?'||c==0){
            size_t end = (c==0)? i : i+1;          /* include terminator */
            while (start<end && buf[start]==' ') start++;
            size_t len = end-start;
            if (len>0){
                char *sent=(char*)malloc(len+1); memcpy(sent,buf+start,len); sent[len]=0;
                /* trim trailing space */
                size_t L=strlen(sent); while(L>0&&sent[L-1]==' ') sent[--L]=0;
                if (strlen(sent)>=6 && !is_pagenumber(sent)) strlist_push(out, sent);
                free(sent);
            }
            start = i+1;
        }
    }
    free(buf);
}
```

- [ ] **Step 4: Run → PASS**

Run: `make pdftest`
Expected: `test_split` passes (4 sentences; de-hyphenation; page-number drop).

- [ ] **Step 5: Checkpoint (no git)** — re-run `make pdftest`.

---

## Task 6: `corpus_store` — persistent append-only + dedup

**Files:**
- Create: `include/corpus/corpus_store.h`, `src/corpus/corpus_store.c`
- Modify: `tests/test_pdf.c`

- [ ] **Step 1: Header**

`include/corpus/corpus_store.h`:
```c
#ifndef CORPUS_STORE_H
#define CORPUS_STORE_H
#include "corpus_split.h"   /* StrList */
/* Append each sentence as a line, skipping exact duplicates already present.
   Returns the number of NEW lines written. Creates the file if absent. */
size_t corpus_append(const char *path, const StrList *sentences);
/* Load the whole store into a growable list (one line per sentence). 0 on success. */
int    corpus_load(const char *path, StrList *out);
/* Number of lines currently stored (0 if absent). */
size_t corpus_count(const char *path);
#endif
```

- [ ] **Step 2: Failing test — idempotent append**

```c
#include "../include/corpus/corpus_store.h"

static void test_store(void) {
    const char *path = "test_corpus_tmp.txt";
    remove(path);
    StrList a; strlist_init(&a); strlist_push(&a,"one sentence here."); strlist_push(&a,"two sentence here.");
    size_t added1 = corpus_append(path, &a);
    CHECK(added1==2, "first append writes 2 lines");
    size_t added2 = corpus_append(path, &a);          /* same content again */
    CHECK(added2==0, "re-appending identical content adds 0 (idempotent)");
    CHECK(corpus_count(path)==2, "count stays 2 after dup append");
    StrList b; strlist_init(&b); strlist_push(&b,"three sentence here.");
    corpus_append(path,&b);
    CHECK(corpus_count(path)==3, "distinct content grows the store");
    StrList loaded; strlist_init(&loaded); corpus_load(path,&loaded);
    CHECK(loaded.count==3, "load returns all 3 lines");
    strlist_free(&a); strlist_free(&b); strlist_free(&loaded);
    remove(path);
}
```
Run `make pdftest` → link error (`corpus_append` undefined).

- [ ] **Step 3: Implement `src/corpus/corpus_store.c`**

```c
#include "../../include/corpus/corpus_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int corpus_load(const char *path, StrList *out) {
    FILE *f = fopen(path, "r"); if (!f) return -1;
    char *line=NULL; size_t cap=0; int c; size_t b=0;
    cap=256; line=(char*)malloc(cap);
    while ((c=fgetc(f))!=EOF){
        if (c=='\n'){ line[b]=0; if(b>0) strlist_push(out,line); b=0; }
        else { if(b+1>=cap){ cap*=2; line=(char*)realloc(line,cap); } line[b++]=(char)c; }
    }
    if (b>0){ line[b]=0; strlist_push(out,line); }
    free(line); fclose(f); return 0;
}

size_t corpus_count(const char *path) {
    StrList s; strlist_init(&s); if (corpus_load(path,&s)!=0) return 0;
    size_t n=s.count; strlist_free(&s); return n;
}

size_t corpus_append(const char *path, const StrList *sentences) {
    StrList existing; strlist_init(&existing); corpus_load(path, &existing); /* ok if absent */
    FILE *f = fopen(path, "a"); if (!f){ strlist_free(&existing); return 0; }
    size_t added=0;
    for (size_t i=0;i<sentences->count;i++){
        const char *s = sentences->lines[i]; int dup=0;
        for (size_t j=0;j<existing.count && !dup;j++) if (strcmp(existing.lines[j],s)==0) dup=1;
        if (!dup){ fprintf(f, "%s\n", s); strlist_push(&existing, s); added++; }
    }
    fclose(f); strlist_free(&existing); return added;
}
```
(Dedup is O(new × existing) exact-match — fine at this scale; a hash set is the future optimization.)

- [ ] **Step 4: Run → PASS**

Run: `make pdftest`
Expected: `test_store` passes (idempotent append; growth; load).

- [ ] **Step 5: Checkpoint (no git)** — re-run `make pdftest`.

---

## Task 7: `pdflearn` driver — growable vocab, replay-train, generate, end-to-end

**Files:**
- Create: `tests/pdflearn_demo.c`
- Modify: `Makefile` (`pdflearn` target)

- [ ] **Step 1: Implement the driver**

`tests/pdflearn_demo.c`:
```c
/* PDF -> persistent corpus -> replay-train cce_wordlm -> generate.
   Usage: ./pdflearn [file.pdf ...]   (no args: uses an embedded sample PDF). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/pdf/pdf_extract.h"
#include "../include/corpus/corpus_split.h"
#include "../include/corpus/corpus_store.h"
#include "../include/cce/cce_wordlm.h"

#define CORPUS_PATH "pdf_corpus.txt"
#define WCTX 3

/* ---- growable string->id vocab (no cap) ---- */
typedef struct { char **w; size_t n, cap; } Vocab;
static void vocab_init(Vocab *v){ v->w=NULL; v->n=0; v->cap=0; }
static int vocab_id(Vocab *v, const char *s, int add){
    for (size_t i=0;i<v->n;i++) if (strcmp(v->w[i],s)==0) return (int)i;
    if (!add) return -1;
    if (v->n==v->cap){ v->cap=v->cap?v->cap*2:256; v->w=(char**)realloc(v->w,v->cap*sizeof(char*)); }
    v->w[v->n]=strdup(s); return (int)v->n++;
}

/* tokenize one sentence (lowercased words + "." token) into ids. */
static int tokenize(Vocab *v, const char *s, int *out, int cap, int add){
    int n=0; char cur[64]; int cl=0;
    for (const char *p=s;;++p){ char c=*p;
        int al=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='\'';
        if (al){ if(cl<63){ cur[cl++]= (c>='A'&&c<='Z')? (char)(c+32):c; } }
        else { if(cl>0){ cur[cl]=0; int id=vocab_id(v,cur,add); if(id>=0&&n<cap)out[n++]=id; cl=0; }
               if(c=='.'||c=='!'||c=='?'){ int id=vocab_id(v,".",add); if(id>=0&&n<cap)out[n++]=id; }
               if(c==0) break; }
    }
    return n;
}

static unsigned char *read_file(const char *path, size_t *len){
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if (sz<=0){ fclose(f); return NULL; }
    unsigned char *buf=(unsigned char*)malloc((size_t)sz);
    size_t rd=fread(buf,1,(size_t)sz,f); fclose(f);
    *len=rd; return buf;
}

/* Embedded sample PDF (uncompressed) so the target runs with no arguments. */
static const char SAMPLE_PDF[] =
"%PDF-1.4\n4 0 obj<</Length 120>>stream\n"
"BT (The little fox found a glowing mushroom in the forest.) Tj ET\n"
"BT (Tom had a black cat named Max who liked to sleep.) Tj ET\n"
"endstream endobj\n";

static const char *STATUS[] = {"OK","ENCRYPTED","UNSUPPORTED_FONTS","NO_TEXT","MALFORMED"};

int main(int argc, char **argv){
    /* 1) ingest: each PDF -> extract -> split -> append to the persistent store. */
    size_t before = corpus_count(CORPUS_PATH);
    size_t ingested = 0;
    if (argc > 1) {
        for (int a=1;a<argc;a++){
            size_t len=0; unsigned char *buf=read_file(argv[a],&len);
            if (!buf){ printf("[ingest] %s: cannot read\n", argv[a]); continue; }
            char *text=(char*)malloc(len*4+16); size_t tlen=0;
            PdfStatus st = pdf_extract_text(buf, len, text, len*4+16, &tlen);
            printf("[ingest] %s: %s (%zu chars)\n", argv[a], STATUS[st], tlen);
            if (st==PDF_OK){ StrList s; strlist_init(&s); corpus_split(text,&s);
                size_t added=corpus_append(CORPUS_PATH,&s); ingested+=added;
                printf("[ingest]   +%zu new sentences\n", added); strlist_free(&s); }
            free(text); free(buf);
        }
    } else {
        StrList s; strlist_init(&s);
        char text[1024]; size_t tlen=0;
        pdf_extract_text((const unsigned char*)SAMPLE_PDF, sizeof(SAMPLE_PDF)-1, text, sizeof(text), &tlen);
        corpus_split(text,&s);
        ingested = corpus_append(CORPUS_PATH,&s);
        printf("[ingest] embedded sample: +%zu new sentences\n", ingested);
        strlist_free(&s);
    }

    /* 2) load the FULL store, build growable vocab, replay-train cce_wordlm. */
    StrList corpus; strlist_init(&corpus);
    if (corpus_load(CORPUS_PATH,&corpus)!=0 || corpus.count==0){ printf("[train] empty corpus\n"); return 1; }
    Vocab v; vocab_init(&v); vocab_id(&v,".",1);
    for (size_t i=0;i<corpus.count;i++){ int t[512]; tokenize(&v,corpus.lines[i],t,512,1); }
    int V=(int)v.n;
    printf("[corpus] sentences=%zu vocab=%d (store had %zu, +%zu this run)\n",
           corpus.count, V, before, ingested);

    cce_wordlm *m = cce_wordlm_create(V, 24, WCTX, 64, 99u);
    for (int epoch=0; epoch<60; epoch++)
        for (size_t i=0;i<corpus.count;i++){ int t[512]; int nt=tokenize(&v,corpus.lines[i],t,512,0);
            for (int j=WCTX;j<nt;j++) cce_wordlm_train_step(m,&t[j-WCTX],t[j],0.02f); }

    /* 3) free-run a sample from the first sentence's opening context. */
    int dot=vocab_id(&v,".",0); int ctx[WCTX];
    { int t[512]; int nt=tokenize(&v,corpus.lines[0],t,512,0);
      for(int k=0;k<WCTX;k++) ctx[k]= (nt>k)? t[k] : 0; }
    char out[1024]=""; for(int k=0;k<WCTX;k++){ strcat(out, v.w[ctx[k]]); strcat(out," "); }
    float *pen=(float*)calloc(V,sizeof(float));
    for (int step=0; step<40 && strlen(out)<900; step++){
        for(int x=0;x<V;x++) pen[x]=0.0f; for(int k=0;k<WCTX;k++) pen[ctx[k]]-=3.0f;
        int nx=cce_wordlm_predict(m,ctx,pen);
        if (nx<0||nx==dot) break;
        strcat(out, v.w[nx]); strcat(out," ");
        for(int k=0;k<WCTX-1;k++) ctx[k]=ctx[k+1];
        ctx[WCTX-1]=nx;
    }
    printf("[generate] %s.\n", out);

    /* 4) self-check (end-to-end gate): store non-empty, vocab grew, generation real. */
    int ok = (corpus.count>0) && (V>3) && (strlen(out) > 8);
    free(pen); cce_wordlm_free(m);
    for (size_t i=0;i<v.n;i++) free(v.w[i]); free(v.w);
    strlist_free(&corpus);
    printf(ok ? "ALL PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
```

- [ ] **Step 2: Add the Makefile `pdflearn` target**

```make
pdflearn: $(PDF_SRC) $(CORPUS_SRC) $(CCE_WORDLM) $(PDFLEARN_DEMO) include/pdf/pdf_extract.h include/pdf/inflate.h include/corpus/corpus_split.h include/corpus/corpus_store.h include/cce/cce_wordlm.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(PDF_SRC) $(CORPUS_SRC) $(CCE_WORDLM) $(PDFLEARN_DEMO) $(LDFLAGS)
	./pdflearn
```
Also add `pdf_corpus.txt` to the `clean` rm list.

- [ ] **Step 3: Build and run (embedded sample)**

Run: `rm -f pdf_corpus.txt; make pdflearn`
Expected: `[ingest] embedded sample: +2 new sentences`, a `[corpus] sentences=2 vocab=… (store had 0, +2 this run)` line, a `[generate] …` real-word line, `ALL PASS`, exit 0.

- [ ] **Step 4: Verify the DYNAMIC property (the headline)**

Run: `./pdflearn` again (no args, re-ingest the same sample)
Expected: `+0 new sentences` (dedup), `store had 2, +0 this run` — store did not grow.

Then create a second tiny PDF and ingest it to prove growth:
```bash
printf '%%PDF-1.4\n4 0 obj<</Length 60>>stream\nBT (A brave knight rode across the green hills today.) Tj ET\nendstream endobj\n' > extra.pdf
./pdflearn extra.pdf
```
Expected: `[ingest] extra.pdf: OK`, `+1 new sentences`, `[corpus] sentences=3 … store had 2, +1 this run` — the corpus and vocab grew, model retrained on all 3. Then `rm -f extra.pdf pdf_corpus.txt`.

- [ ] **Step 5: Regression check** — Run `make pdftest` (all module units still green) and confirm no other target broke (changes are additive).

- [ ] **Step 6: Checkpoint (no git)** — re-run `make pdflearn`; confirm `ALL PASS`.

---

## Self-Review (against the spec)

**Spec coverage:**
- §3.1 inflate + `pdf_flate_decode` → Task 1. ✓
- §3.2 `pdf_extract` (streams, Tj/TJ, literals/hex, space heuristic, detect-and-skip) → Tasks 2–4. ✓
- §3.3 `corpus_split` (StrList, de-hyphenate, sentence split, junk drop) → Task 5. ✓
- §3.4 `corpus_store` (append/load/count, dedup) → Task 6. ✓
- §3.5 driver (growable vocab, sized-to-vocab model, streamed replay training, sample PDF, stats) → Task 7. ✓
- §4 dynamic guarantees (persistent, no caps, replay) → Task 6 + Task 7 Step 4 (explicit growth check). ✓
- §5 testing (embedded inflate vector, embedded PDFs incl. encrypted-skip, split/store units, end-to-end) → Tasks 1–7. ✓
- §6 no contracts → respected (no contract code anywhere). ✓
- §7 out-of-scope respected (CID/OCR/encrypted skipped; replay only; no HTTP). ✓

**Placeholder scan:** No TBD/TODO. Two steps embed offline-generated byte vectors — each gives the **exact** PowerShell command to produce them and the exact spot to paste; that is a concrete action, not a placeholder. The inflate body is specified as the standard RFC 1951 decoder behind a fixed interface, gated by an exact test vector (Task 1 note).

**Type consistency:** `StrList` defined once in `corpus_split.h`, reused by `corpus_store.h` and the driver. `PdfStatus` enum order matches the `STATUS[]` table in the driver. `pdf_extract_text`, `corpus_split`, `corpus_append/load/count`, `cce_wordlm_*` signatures match their headers across tasks. `SPACE_KERN`, `CORPUS_PATH`, `WCTX` used consistently.

---

## Execution note

Recommended: inline execution via `superpowers:executing-plans`. The inflater (Task 1) is the one risky piece — it is test-gated, so iterate against the embedded vector until exact. Everything else is small, self-contained C with deterministic unit tests.

---

## Implementation notes (executed inline — 2026-06-26)

All tasks landed; `make pdftest` = **19/19 checks**, `make pdflearn` = `ALL PASS`, dynamic
growth + dedup verified. Notes:

- **Inflater passed the embedded vector first try** — the puff (Mark Adler, public-domain)
  reproduction was correct without debugging.
- **Tasks 2–6 batched.** The extractor (Tasks 2–4, one file) and the corpus modules
  (Tasks 5–6) were written and verified together in one `make pdftest` cycle rather than
  per-task — leaner, and the per-`CHECK` output still localizes any failure.
- **Test vectors** generated offline with PowerShell `System.IO.Compression.DeflateStream`
  (raw DEFLATE). For the `/FlateDecode` case, the zlib header `0x78,0x9c` is prepended; the
  adler32 tail is omitted (puff stops at the final block, so it is never consumed).
- **Dynamic property proven:** re-ingesting the embedded sample → `+0` (dedup, store stays
  2 / vocab 20); ingesting a distinct PDF → `+1`, store `2→3`, vocab `20→27`, replay-trained
  on all three.
- **Honesty:** at this tiny corpus scale the LM reproduces corpus sentences (memorized
  transitions — same data-sparsity note as the `cce_wordlm` work). The pipeline is what
  unlocks a real, large corpus; that is the point. CID/Type0-font and scanned PDFs are
  detected-and-skipped, not handled.
- Changes are **purely additive**: new `src/pdf/`, `src/corpus/`, `include/pdf/`,
  `include/corpus/`, `tests/test_pdf.c`, `tests/pdflearn_demo.c`, and the `pdftest` /
  `pdflearn` make targets. No existing source modified.
