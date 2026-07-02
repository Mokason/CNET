# Extraction Quality (font /Differences recovery + quality filter) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Recover garbled PDF text by decoding the font `/Differences` encoding (no `/ToUnicode` in the book) via per-stream best-match, and drop residual junk with an English-likeness quality filter — so recall and graduation see clean prose.

**Architecture:** A new `font_decode` module parses `/Differences` arrays into `code→text` maps, maps glyph names → text (AGL-lite incl. ligatures), and decodes each content stream with whichever candidate map (or raw WinAnsi) scores highest on English-likeness. `pdf_extract` is refactored to extract raw codes then best-match-decode. `corpus_quality_keep` drops junk. Benchmarks + a real-book before/after + a Q&A probe. Zero core edits.

**Tech Stack:** C11 (gcc, `-mno-avx`), stdlib only.

**Project note — NO GIT:** `.git` removed. Run no git commands. "Commit" → checkpoint: re-run the relevant `make` target.

---

## File Structure

- **Create `include/pdf/font_decode.h`, `src/pdf/font_decode.c`** — `/Differences` parse, glyph→text, `decode_codes`, `english_likeness`, `font_decode_pick_best`.
- **Modify `src/pdf/pdf_extract.c`** — raw `emit`; parse `FontDiffs` once; per-stream raw-codes → best-match decode.
- **Modify `include/corpus/corpus_split.h`, `src/corpus/corpus_split.c`** — `corpus_quality_keep`.
- **Create `tests/test_font_decode.c`** — unit tests + real-book before/after + Q&A probe + benchmarks.
- **Modify `Makefile`** — add `font_decode.c` to `PDF_SRC`; add `fontdecode` target/PHONY/clean.

`CHECK` macro in the test:
```c
static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)
```

---

## Task 1: `font_decode` module + `corpus_quality_keep` + unit tests

**Files:**
- Create: `include/pdf/font_decode.h`, `src/pdf/font_decode.c`, `tests/test_font_decode.c`
- Modify: `include/corpus/corpus_split.h`, `src/corpus/corpus_split.c`, `Makefile`

- [ ] **Step 1: Header `include/pdf/font_decode.h`**

```c
#ifndef PDF_FONT_DECODE_H
#define PDF_FONT_DECODE_H
#include <stddef.h>

typedef struct { unsigned char set[256]; char text[256][8]; } DiffMap;
typedef struct { DiffMap maps[16]; int n; } FontDiffs;

/* Map a PDF glyph name to ASCII text ("f_i"->"fi", "space"->" ", "A"->"A",
   "uniXXXX"->char; unknown -> ""). */
void   glyph_to_text(const char *name, char *out, size_t cap);
/* Parse every /Differences array in the raw PDF into a code->text map (<=16). */
void   font_diffs_parse(const unsigned char *pdf, size_t n, FontDiffs *fd);
/* Decode raw code bytes through `map` (NULL = WinAnsi: keep printable ASCII,
   drop the rest). Returns bytes written; out is nul-terminated. */
size_t decode_codes(const unsigned char *raw, size_t n, const DiffMap *map, char *out, size_t cap);
/* Fraction of alpha tokens that look like words (len>=2 with a vowel). */
double english_likeness(const char *s);
/* Decode raw codes with the candidate (WinAnsi or a Differences map) that
   maximizes english_likeness; writes the winner to out. Returns its length. */
size_t font_decode_pick_best(const unsigned char *raw, size_t rawlen, const FontDiffs *fd, char *out, size_t cap);
#endif
```

- [ ] **Step 2: Implement `src/pdf/font_decode.c`**

```c
#include "../../include/pdf/font_decode.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

void glyph_to_text(const char *name, char *out, size_t cap){
    if(cap) out[0]=0;
    if(!name||!*name||cap<2) return;
    if(strchr(name,'_')){                         /* ligature/compound: split */
        size_t o=0; const char *p=name;
        while(*p && o+1<cap){ const char *st=p; while(*p && *p!='_') p++;
            char part[64]; int len=(int)(p-st); if(len>63) len=63; memcpy(part,st,len); part[len]=0;
            char t[8]; glyph_to_text(part,t,sizeof(t));
            for(int k=0;t[k]&&o+1<cap;k++) out[o++]=t[k];
            if(*p=='_') p++; }
        out[o]=0; return;
    }
    if(strlen(name)==1){ char c=name[0];
        if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')){ out[0]=c; out[1]=0; return; } }
    static const struct { const char*n; const char*t; } T[] = {
        {"space"," "},{"period","."},{"comma",","},{"hyphen","-"},{"colon",":"},{"semicolon",";"},
        {"exclam","!"},{"question","?"},{"parenleft","("},{"parenright",")"},{"quotesingle","'"},
        {"quotedbl","\""},{"slash","/"},{"percent","%"},{"ampersand","&"},{"endash","-"},{"emdash","-"},
        {"quoteright","'"},{"quoteleft","'"},{"quotedblleft","\""},{"quotedblright","\""},{"bullet","-"},
        {"zero","0"},{"one","1"},{"two","2"},{"three","3"},{"four","4"},{"five","5"},
        {"six","6"},{"seven","7"},{"eight","8"},{"nine","9"},
        {"fi","fi"},{"fl","fl"},{"ff","ff"},{"ffi","ffi"},{"ffl","ffl"},{0,0} };
    for(int i=0;T[i].n;i++) if(strcmp(T[i].n,name)==0){ snprintf(out,cap,"%s",T[i].t); return; }
    if(strncmp(name,"uni",3)==0 && strlen(name)>=7){ int v=0,ok=1;
        for(int k=3;k<7;k++){ char c=name[k]; int d=(c>='0'&&c<='9')?c-'0':(c>='A'&&c<='F')?c-'A'+10:(c>='a'&&c<='f')?c-'a'+10:-1;
            if(d<0){ ok=0; break; } v=v*16+d; }
        if(ok && v>=0x20 && v<0x7f){ out[0]=(char)v; out[1]=0; return; } }
    out[0]=0;   /* unknown -> drop */
}

void font_diffs_parse(const unsigned char *pdf, size_t n, FontDiffs *fd){
    fd->n=0;
    const char *p=(const char*)pdf;
    for(size_t i=0;i+12<n && fd->n<16;i++){
        if(memcmp(p+i,"/Differences",12)!=0) continue;
        size_t j=i+12; while(j<n && p[j]!='[' && p[j]!='>') j++;
        if(j>=n || p[j]!='[') continue;
        j++;
        DiffMap *dm=&fd->maps[fd->n]; memset(dm,0,sizeof(*dm));
        int code=0;
        while(j<n && p[j]!=']'){
            if(p[j]==' '||p[j]=='\r'||p[j]=='\n'||p[j]=='\t'){ j++; continue; }
            if(p[j]>='0'&&p[j]<='9'){ code=0; while(j<n&&p[j]>='0'&&p[j]<='9'){ code=code*10+(p[j]-'0'); j++; } }
            else if(p[j]=='/'){ j++; char name[64]; int k=0;
                while(j<n && p[j]!='/' && p[j]!=']' && p[j]!=' ' && p[j]!='\r' && p[j]!='\n' && p[j]!='\t'){ if(k<63) name[k++]=p[j]; j++; }
                name[k]=0;
                if(code>=0 && code<256){ char t[8]; glyph_to_text(name,t,sizeof(t));
                    if(t[0]){ dm->set[code]=1; snprintf(dm->text[code],8,"%s",t); } code++; }
            }
            else j++;
        }
        fd->n++;
    }
}

size_t decode_codes(const unsigned char *raw, size_t n, const DiffMap *map, char *out, size_t cap){
    size_t o=0;
    for(size_t i=0;i<n;i++){ unsigned char c=raw[i];
        if(map && map->set[c]){ const char *t=map->text[c]; for(size_t k=0;t[k];k++) if(o+1<cap) out[o++]=t[k]; }
        else if((c>=0x20&&c<0x7f)||c=='\n'||c=='\r'||c=='\t'){ if(o+1<cap) out[o++]=(char)c; }
    }
    if(o<cap) out[o]=0; return o;
}

double english_likeness(const char *s){
    int words=0, toks=0;
    for(const char *p=s; *p; ){
        while(*p && !isalpha((unsigned char)*p)) p++;
        if(!*p) break;
        const char *st=p; int hasv=0;
        while(isalpha((unsigned char)*p)){ char c=(char)tolower((unsigned char)*p); if(strchr("aeiouy",c)) hasv=1; p++; }
        int len=(int)(p-st); toks++; if(len>=2 && hasv) words++;
    }
    return toks? (double)words/toks : 0.0;
}

size_t font_decode_pick_best(const unsigned char *raw, size_t rawlen, const FontDiffs *fd, char *out, size_t cap){
    static char cand[1<<20];
    size_t bl=decode_codes(raw,rawlen,NULL,out,cap);   /* WinAnsi into out */
    double best=english_likeness(out);
    for(int m=0; m<fd->n; m++){
        size_t cl=decode_codes(raw,rawlen,&fd->maps[m],cand,sizeof(cand));
        double sc=english_likeness(cand);
        if(sc>best){ best=sc; size_t k=cl<cap-1?cl:(cap>0?cap-1:0); memcpy(out,cand,k); out[k]=0; bl=k; }
    }
    return bl;
}
```

- [ ] **Step 3: `corpus_quality_keep` (header + impl)**

In `include/corpus/corpus_split.h`, after `corpus_split`:
```c
/* English-likeness gate: keep a sentence iff real-word fraction >= 0.6, token
   count >= 3, and alpha+space ratio >= 0.85. Returns 1 to keep, 0 to drop. */
int corpus_quality_keep(const char *sentence);
```
In `src/corpus/corpus_split.c` (after `corpus_split`):
```c
int corpus_quality_keep(const char *s){
    size_t alpha_space=0,total=0;
    for(const char*p=s;*p;p++){ total++; if(isalpha((unsigned char)*p)||*p==' ') alpha_space++; }
    int words=0,toks=0;
    for(const char *p=s; *p; ){
        while(*p && !isalpha((unsigned char)*p)) p++;
        if(!*p) break;
        const char *st=p; int hasv=0;
        while(isalpha((unsigned char)*p)){ char c=(char)tolower((unsigned char)*p); if(strchr("aeiouy",c)) hasv=1; p++; }
        int len=(int)(p-st); toks++; if(len>=2 && hasv) words++;
    }
    double rw = toks? (double)words/toks : 0.0;
    double ar = total? (double)alpha_space/total : 0.0;
    return (toks>=3 && rw>=0.6 && ar>=0.85) ? 1 : 0;
}
```

- [ ] **Step 4: Unit tests `tests/test_font_decode.c` + Makefile**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/pdf/font_decode.h"
#include "../include/corpus/corpus_split.h"

static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)

static void test_glyph(void){
    char t[8];
    glyph_to_text("f_i",t,sizeof(t));   CHECK(strcmp(t,"fi")==0,"f_i -> fi");
    glyph_to_text("f_f_i",t,sizeof(t)); CHECK(strcmp(t,"ffi")==0,"f_f_i -> ffi");
    glyph_to_text("T_h",t,sizeof(t));   CHECK(strcmp(t,"Th")==0,"T_h -> Th");
    glyph_to_text("space",t,sizeof(t)); CHECK(strcmp(t," ")==0,"space -> ' '");
    glyph_to_text("A",t,sizeof(t));     CHECK(strcmp(t,"A")==0,"A -> A");
    glyph_to_text("bogusname",t,sizeof(t)); CHECK(t[0]==0,"unknown -> drop");
}
static void test_diffs(void){
    const char *pdf = "x /Differences[31/f_i 65/A/B] y";
    FontDiffs fd; font_diffs_parse((const unsigned char*)pdf, strlen(pdf), &fd);
    CHECK(fd.n==1,"parsed one Differences map");
    CHECK(fd.n>=1 && fd.maps[0].set[31] && strcmp(fd.maps[0].text[31],"fi")==0,"code 31 -> fi");
    CHECK(fd.n>=1 && fd.maps[0].set[66] && strcmp(fd.maps[0].text[66],"B")==0,"code 66 -> B (auto-increment)");
}
static void test_decode_pick(void){
    /* raw codes: byte 31 is a ligature, rest ascii "speci" + 31 + "c" */
    unsigned char raw[]={'s','p','e','c','i',31,'c'};
    FontDiffs fd; const char *pdf="/Differences[31/f_i]"; font_diffs_parse((const unsigned char*)pdf,strlen(pdf),&fd);
    char out[64];
    decode_codes(raw,sizeof(raw),NULL,out,sizeof(out));       /* WinAnsi drops 31 */
    CHECK(strcmp(out,"specic")==0,"raw WinAnsi drops the ligature byte -> specic");
    font_decode_pick_best(raw,sizeof(raw),&fd,out,sizeof(out));
    CHECK(strcmp(out,"specific")==0,"best-match recovers the ligature -> specific");
}
static void test_quality(void){
    CHECK(corpus_quality_keep("the team builds the system and ships it")==1,"keep a real sentence");
    CHECK(corpus_quality_keep("3 1 4 5")==0,"drop a page-number fragment");
    CHECK(corpus_quality_keep(":<ZhymmQS>%Uyew>H8Dq?")==0,"drop symbol-soup");
    CHECK(corpus_quality_keep("P 49 Q Miao Song R S T")==0,"drop stray-letter TOC noise");
}

int main(void){
    printf("=== test_font_decode ===\n");
    test_glyph();
    test_diffs();
    test_decode_pick();
    test_quality();
    printf("%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}
```
Makefile: change `PDF_SRC` to include `font_decode.c`:
```make
PDF_SRC := src/pdf/inflate.c src/pdf/pdf_extract.c src/pdf/font_decode.c
```
Add vars + target:
```make
FONTDECODE_TEST := tests/test_font_decode.c
fontdecode: $(PDF_SRC) $(CORPUS_SRC) $(TILEMEM_SRC) $(FONTDECODE_TEST) include/pdf/font_decode.h include/pdf/pdf_extract.h include/corpus/corpus_split.h include/corpus/tile_memory.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(PDF_SRC) $(CORPUS_SRC) $(TILEMEM_SRC) $(FONTDECODE_TEST) $(LDFLAGS)
	./fontdecode
```
Add `fontdecode` to `.PHONY` and the `clean` rm list; add `font_qa` to the `rm -rf` line.

- [ ] **Step 5: Build and run the unit tests**

Run: `make fontdecode`
Expected: `test_glyph`/`test_diffs`/`test_decode_pick`/`test_quality` pass; `12/12 checks passed` (or current count); exit 0. The decode_pick test proves `specic`→`specific` recovery in isolation.

- [ ] **Step 6: Checkpoint (no git)** — re-run `make fontdecode`.

---

## Task 2: wire font decode into `pdf_extract` + real-book proof + benchmarks

**Files:**
- Modify: `src/pdf/pdf_extract.c`, `tests/test_font_decode.c`

- [ ] **Step 1: Refactor `pdf_extract.c` — raw `emit` + best-match per stream**

Add the include and change `emit` to NOT filter (raw byte):
```c
#include "../../include/pdf/font_decode.h"
```
Replace the `emit` function body:
```c
/* Emit a raw code byte (the WinAnsi/Differences decode + ASCII filter happens
   later in decode_codes / font_decode_pick_best). NUL is dropped to keep
   strlen-based consumers safe. */
static void emit(char *out, size_t cap, size_t *o, char c) {
    if (c == 0) return;
    if (*o + 1 < cap) out[(*o)++] = c;
}
```
Replace the stream loop body of `pdf_extract_text` (the `for (size_t i = 0; i + 6 < n; )` block) so each content stream's raw codes are extracted then best-match-decoded:
```c
    FontDiffs fd; font_diffs_parse(pdf, n, &fd);
    static unsigned char codes[1<<20];

    for (size_t i = 0; i + 6 < n; ) {
        if (memcmp(p+i, "stream", 6) == 0) {
            size_t s0 = i + 6;
            if (s0 < n && p[s0]=='\r') s0++;
            if (s0 < n && p[s0]=='\n') s0++;
            size_t e = s0;
            while (e + 9 < n && memcmp(p+e, "endstream", 9) != 0) e++;

            int is_flate = 0;
            size_t lo = (i > 200) ? i - 200 : 0;
            for (size_t j = lo; j + 12 < i; j++)
                if (memcmp(p+j, "/FlateDecode", 12) == 0) { is_flate = 1; break; }

            const unsigned char *cs = NULL; size_t cslen = 0;
            static unsigned char dec[1<<20];
            if (is_flate) {
                long dl = pdf_flate_decode(pdf + s0, e - s0, dec, sizeof(dec));
                if (dl > 0 && looks_like_content(dec, (size_t)dl)) { cs = dec; cslen = (size_t)dl; }
            } else if (looks_like_content(pdf + s0, e - s0)) {
                cs = pdf + s0; cslen = e - s0;
            }
            if (cs) {
                size_t clen = 0;
                process_content(cs, cslen, (char*)codes, sizeof(codes), &clen);   /* raw codes */
                if (o < cap) o += font_decode_pick_best(codes, clen, &fd, out + o, cap - o);
            }
            i = e + 9;
        } else i++;
    }
```
(`process_content` is unchanged in logic; with the new raw `emit` it now writes raw code bytes into `codes`.)

- [ ] **Step 2: Regression — synthetic PDFs unchanged**

Run: `make pdftest`
Expected: `19/19 checks passed`. (The synthetic PDFs have no `/Differences`; best-match picks raw WinAnsi → identical output → `Hello World`/`Hello Flate` still extracted.)

- [ ] **Step 3: Real-book before/after + Q&A probe + benchmarks (append to `tests/test_font_decode.c`)**

Add includes at the top: `#include "../include/pdf/pdf_extract.h"`, `#include "../include/corpus/tile_memory.h"`, `#include <time.h>`.
Add before `main` and call from `main`:
```c
static unsigned char *readfile(const char *p,size_t*len){ FILE*f=fopen(p,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0){fclose(f);return NULL;}
    unsigned char*b=malloc(sz); *len=fread(b,1,sz,f); fclose(f); return b; }
static void rm_store(const char*d){ char p[300]; snprintf(p,sizeof(p),"%s/hot.bin",d); remove(p);
    snprintf(p,sizeof(p),"%s/warm.bin",d); remove(p); }

static void test_real_book(void){
    const char *path="C:/Users/Mokason/Downloads/aivalueplaybook.pdf";
    FILE *pr=fopen(path,"rb"); if(!pr){ printf("[book] not found - skipping\n"); return; } fclose(pr);
    size_t len=0; unsigned char *buf=readfile(path,&len);
    char *text=malloc(len*4+16); size_t tl=0;
    clock_t t0=clock();
    PdfStatus st=pdf_extract_text(buf,len,text,len*4+16,&tl);
    double ms=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;
    CHECK(st==PDF_OK,"book extracted");
    StrList s; strlist_init(&s); corpus_split(text,&s);

    /* quality split */
    int kept=0,dropped=0; double goodsum=0;
    for(size_t i=0;i<s.count;i++){ if(corpus_quality_keep(s.lines[i])) kept++; else dropped++;
        goodsum += english_likeness(s.lines[i]); }
    double goodfrac = s.count? goodsum/s.count : 0;

    /* ligature recovery signal: words that were broken by dropped ligatures */
    const char *lig[]={"specific","efficient","first","field","final","office","benefit","define","difficult","financial","fifty"};
    int recovered=0; for(int k=0;k<(int)(sizeof(lig)/sizeof(lig[0]));k++) if(strstr(text,lig[k])) recovered++;

    printf("\n[bench] extract+decode %.0f ms, %zu chars\n", ms, tl);
    printf("[book] sentences=%zu  mean english-likeness=%.2f  quality-kept=%d dropped=%d\n",
           s.count, goodfrac, kept, dropped);
    printf("[book] ligature words recovered (of 11 probes): %d\n", recovered);
    CHECK(goodfrac>0.6,"corpus is mostly English-like after decode");
    CHECK(recovered>0,"at least one ligature word recovered (specific/first/...)");
    CHECK(dropped>0,"quality filter dropped some junk");

    /* Q&A probe: ingest QUALITY-FILTERED tiles, ask the storytelling question */
    rm_store("font_qa");
    TileMemory *m=tilemem_open("font_qa",256,9000,0.6);
    for(size_t i=0;i<s.count;i++) if(corpus_quality_keep(s.lines[i])) tilemem_ingest(m,s.lines[i],s.lines[i],"","book");
    TileHit h[1]; int nh=tilemem_search(m,"why does storytelling and alignment matter",1,h,1);
    if(nh>0){ double topq=english_likeness(h[0].value);
        printf("[qa] storytelling top hit (likeness %.2f): %.80s\n", topq, h[0].value);
        CHECK(topq>0.5,"storytelling top hit is real prose, not symbol-soup"); }
    tilemem_close(m);
    free(text); free(buf); strlist_free(&s);
}
```
Add `test_real_book();` to `main` before the summary print.

- [ ] **Step 4: Build and run (acceptance + benchmarks)**

Run: `make fontdecode; echo "exit=$?"`
Expected: unit tests pass; `[bench] extract+decode … ms`; `[book] … mean english-likeness=… quality-kept=… dropped=…`; `[book] ligature words recovered … : N` (N>0); `[qa] storytelling top hit (likeness >0.5): <real sentence>`; all checks pass; `exit=0`.
If `recovered==0`: the book's ligature codes may differ — print a few `[book]` sample sentences and confirm a `/Differences` map was parsed (`font_diffs_parse` n>0); widen the `lig[]` probe set.

- [ ] **Step 5: Downstream regression** — Run `make tiermem_test`, `make graduate`, `make pdftest`; all still green (improved extraction, range-based assertions hold).

- [ ] **Step 6: Checkpoint (no git)** — re-run `make fontdecode`.

---

## Self-Review (against the spec)

**Spec coverage:**
- §1.1 font-encoding recovery (Differences + glyph→text + best-match) → Task 1 (module) + Task 2 (wired). ✓
- §1.2 quality filter → Task 1 `corpus_quality_keep`. ✓
- §1.3 measured improvement (good-fraction, ligature, storytelling Q&A) → Task 2 `test_real_book`. ✓
- §1.4 no regression → Task 2 Step 2/5 (`pdftest`/`tiermem_test`/`graduate`). ✓
- §1.5 benchmarks → Task 2 `[bench]`/`[book]` prints. ✓
- §3 mechanism (parse, glyph→text, per-stream best-match) → Tasks 1–2. ✓
- §4 components (`font_decode.c`, pdf_extract refactor, `corpus_quality_keep`, test) → Tasks 1–2. ✓
- §8 limits respected (heuristic best-match; ObjStm object-graph deferred). ✓

**Placeholder scan:** none — complete code + exact commands. The `recovered==0` note is a diagnostic contingency, not a placeholder.

**Type consistency:** `DiffMap`/`FontDiffs`, `glyph_to_text`/`font_diffs_parse`/`decode_codes`/`english_likeness`/`font_decode_pick_best`, `corpus_quality_keep` signatures match the headers across tasks; `process_content`/`emit` refactor preserves the `(out,cap,o)` contract; `PDF_SRC` now includes `font_decode.c` so every consumer links it.

---

## Execution note

Recommended: inline execution via `superpowers:executing-plans`. Task 1 is unit-testable in isolation (proves `specic`→`specific`); Task 2 wires it and proves it on the book. The real-book task auto-skips if the PDF is absent. Adding `font_decode.c` to `PDF_SRC` means every extraction consumer (pdftest/tiermem/graduate/pdflearn) relinks with it — they stay green because synthetic PDFs have no `/Differences` and real-book assertions are range-based.

---

## Implementation notes (executed inline — 2026-06-26)

`make fontdecode` = **20/20**, warning-clean; downstream all green after relinking
`PDF_SRC`: `pdftest` 19/19, `tiermem_test` 18/18, `graduate` 9/9 (backward-compatible on
synthetic PDFs, improved on the real book).

**Measured (real book):** extract+decode **114 ms**; mean english-likeness **0.81**;
quality filter **dropped 1410** junk sentences (kept 5563); **10/11 ligature probe words
recovered** (`specific`,`first`,`field`,…); the storytelling Q&A top hit went from
`:<ZhymmQS>%Uyew>H8Dq?` to real prose (*"Some of them are clear, such as with SonoSim's
need for information retrieval"*, likeness 1.0). Q&A re-run confirms *"The **first** is
our platform **offering**"* (was "rst"/"oering").

**Deviation:** `english_likeness` alone can't tell a 1-word `specic` from `specific` (both
score 1.0), so the unit test failed at first. Fix: added a **recovered-glyph-count
tiebreak** to `font_decode_pick_best` (on a likeness tie, prefer the decode that mapped
more font codes). Real-book result unchanged.

**Honest:** apostrophes are still dropped (`thats`,`weve`) — `quoteright` not in the
parsed Differences for those streams; the `"last mile"` conceptual miss is **Lever 2**
(semantic retrieval), not extraction. Best-match is a heuristic (no font tracking); full
per-font association via `ObjStm` object-graph is deferred. Purely additive: new
`font_decode.c`, `corpus_quality_keep`, `test_font_decode.c`, the `fontdecode` target; a
moderate `pdf_extract.c` refactor (raw codes + best-match).
