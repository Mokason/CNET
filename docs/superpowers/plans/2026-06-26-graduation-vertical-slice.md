# Graduation Vertical Slice Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the loop end-to-end — mine a near-deterministic regularity from book memory, distill it into two frozen `btn_certify`-proven positionally-tagged units, register them, evict the source tiles from the fuzzy memory, and have the router auto-compose the units into the answer (with benchmarks).

**Architecture:** A real `graduate_deterministic()` operation mines dominant-next bigrams, builds two tiny BTNs (same map, ports tagged `w_a→w_b` and `w_b→w_c`), certifies + `registry_add_certified`s them, and evicts matching tiles. A test drives the full loop, asserts `route_plan` discovers the 2-hop and `route_execute` reconstructs the run, prints a benchmark table, and runs on a controlled corpus (guaranteed) + the real book (honest). Consumes public `nn`/`contract`/`router`/`tile_memory` APIs; zero core edits.

**Tech Stack:** C11 (gcc, `-mno-avx`), stdlib + libm.

**Project note — NO GIT:** `.git` removed. Run no git commands. "Commit" → checkpoint: re-run `make graduate`.

---

## File Structure

- **Modify `include/corpus/tile_memory.h`, `src/corpus/tile_memory.c`** — add `tilemem_evict_containing`.
- **Create `include/corpus/graduate.h`, `src/corpus/graduate.c`** — the graduation operation.
- **Create `tests/test_graduate.c`** — full loop + router composition + benchmark table.
- **Modify `Makefile`** — `GRADUATE_SRC`/`GRADUATE_TEST` vars, `graduate` target, `.PHONY`, `clean`.

`CHECK` macro in the test:
```c
static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)
```

---

## Task 1: `tilemem_evict_containing` (memory shrinks as knowledge graduates)

**Files:**
- Modify: `include/corpus/tile_memory.h`, `src/corpus/tile_memory.c`

- [ ] **Step 1: Header — add the declaration**

In `include/corpus/tile_memory.h`, after `tilemem_certifiable`:
```c
/* Evict HOT tiles whose key contains `needle` (graduated knowledge leaves the
   fuzzy memory). Returns the number evicted. */
size_t tilemem_evict_containing(TileMemory *m, const char *needle);
```

- [ ] **Step 2: Implement in `src/corpus/tile_memory.c`** (after `tilemem_certifiable`)

```c
size_t tilemem_evict_containing(TileMemory *m, const char *needle){
    if(!needle||!*needle) return 0;
    size_t ev=0;
    for(int i=0;i<m->n_hot;){
        if(strstr(m->hot[i].key, needle)){ tile_free(&m->hot[i]); m->hot[i]=m->hot[--m->n_hot]; ev++; }
        else i++;
    }
    return ev;
}
```

- [ ] **Step 3: Verify it builds (via the existing suite)**

Run: `make tiermem_test`
Expected: still `18/18 checks passed` (the new symbol compiles; no behavior change to existing tests).

- [ ] **Step 4: Checkpoint (no git)** — re-run `make tiermem_test`.

---

## Task 2: `graduate_deterministic` — mine → certify → register → evict

**Files:**
- Create: `include/corpus/graduate.h`, `src/corpus/graduate.c`

- [ ] **Step 1: Header `include/corpus/graduate.h`**

```c
#ifndef CORPUS_GRADUATE_H
#define CORPUS_GRADUATE_H
#include "../nn.h"
#include "../router.h"
#include "../contract/contract.h"
#include "corpus_split.h"
#include "tile_memory.h"

typedef struct {
    int    ok;                  /* 1 if a 2-step run X->Y->Z was graduated */
    int    vocab;               /* scoped vocab V (BTN dim) */
    int    exemplars;           /* decidable-core bigram exemplars (sources) */
    char   seed[64], mid[64], dst[64];   /* X, Y, Z */
    int    seed_idx, dst_idx;   /* scoped one-hot indices of X and Z */
    int    units;               /* certified units registered */
    int    cert_a, cert_b;      /* btn_certify result (0 = certified) */
    size_t tiles_evicted, mem_before, mem_after;
    double ms_mine, ms_build_certify, ms_register, ms_evict;
} GraduateReport;

/* Owns the two BTNs + contracts + shared exemplar tables; the registry borrows the
   BTN pointers, so keep this alive until after routing, then free it. */
typedef struct {
    BinaryTransformNetwork btn_a, btn_b;
    Contract con_a, con_b;
    double *X, *Y;
    char tag_a[32], tag_b[32], tag_c[32];
    int vocab, seed_idx, dst_idx, valid;
} GraduatedUnits;

/* Mine near-deterministic bigrams from `corpus`, distill+certify two positionally
   tagged units computing the dominant-next map, register them certified in `reg`,
   and evict tiles containing the seed word from `mem`. Returns 0 on a 2-hop
   graduation, -1 otherwise (rep still filled). Caller frees gu after routing. */
int graduate_deterministic(const StrList *corpus, PrimitiveRegistry *reg, TileMemory *mem,
                           int min_count, double min_share, int max_sources,
                           GraduatedUnits *gu, GraduateReport *rep);
void graduated_units_free(GraduatedUnits *gu);
#endif
```

- [ ] **Step 2: Implement `src/corpus/graduate.c`**

```c
#include "../../include/corpus/graduate.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* --- local string->id vocab over the whole corpus --- */
typedef struct { char **w; int n, cap; } Vocab;
static int v_id(Vocab *v, const char *s, int add){
    for(int i=0;i<v->n;i++) if(strcmp(v->w[i],s)==0) return i;
    if(!add) return -1;
    if(v->n==v->cap){ v->cap=v->cap?v->cap*2:256; v->w=(char**)realloc(v->w,(size_t)v->cap*sizeof(char*)); }
    v->w[v->n]=strdup(s); return v->n++;
}
static int v_tok(Vocab *v, const char *s, int *out, int cap){ int n=0; char cur[64]; int cl=0;
    for(const char *p=s;;p++){ char c=*p; int al=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='\'';
        if(al){ if(cl<63) cur[cl++]=(c>='A'&&c<='Z')?(char)(c+32):c; }
        else { if(cl>0){ cur[cl]=0; int id=v_id(v,cur,1); if(id>=0&&n<cap) out[n++]=id; cl=0; }
               if(c=='.'||c=='!'||c=='?'){ int id=v_id(v,".",1); if(id>=0&&n<cap) out[n++]=id; }
               if(c==0) break; } }
    return n;
}
/* per-word continuation counts */
typedef struct { int *nx,*cn,n,cap; } Conts;
static void c_add(Conts *c, int nx){
    for(int i=0;i<c->n;i++) if(c->nx[i]==nx){ c->cn[i]++; return; }
    if(c->n==c->cap){ c->cap=c->cap?c->cap*2:4; c->nx=(int*)realloc(c->nx,(size_t)c->cap*sizeof(int)); c->cn=(int*)realloc(c->cn,(size_t)c->cap*sizeof(int)); }
    c->nx[c->n]=nx; c->cn[c->n]=1; c->n++;
}

void graduated_units_free(GraduatedUnits *gu){
    if(!gu||!gu->valid) return;
    contract_free(&gu->con_a); contract_free(&gu->con_b);
    btn_free(&gu->btn_a); btn_free(&gu->btn_b);
    free(gu->X); free(gu->Y); gu->valid=0;
}

int graduate_deterministic(const StrList *corpus, PrimitiveRegistry *reg, TileMemory *mem,
                           int min_count, double min_share, int max_sources,
                           GraduatedUnits *gu, GraduateReport *rep){
    memset(rep,0,sizeof(*rep)); memset(gu,0,sizeof(*gu));
    clock_t t0=clock();

    /* 1) tokenize whole corpus, count bigrams */
    Vocab voc={0};
    Conts *cont=NULL; int vcap=0;
    int prev=-1;
    for(size_t s=0;s<corpus->count;s++){
        int t[2048]; int nt=v_tok(&voc,corpus->lines[s],t,2048);
        if(voc.n>vcap){ int old=vcap; vcap=voc.n; cont=(Conts*)realloc(cont,(size_t)vcap*sizeof(Conts));
            for(int i=old;i<vcap;i++){ cont[i].nx=NULL; cont[i].cn=NULL; cont[i].n=0; cont[i].cap=0; } }
        prev=-1;
        for(int i=0;i<nt;i++){ if(prev>=0){ if(voc.n>vcap){ int old=vcap; vcap=voc.n; cont=(Conts*)realloc(cont,(size_t)vcap*sizeof(Conts)); for(int k=old;k<vcap;k++){cont[k].nx=NULL;cont[k].cn=NULL;cont[k].n=0;cont[k].cap=0;} } c_add(&cont[prev], t[i]); } prev=t[i]; }
    }
    /* 2) sources: dominant share >= min_share, total >= min_count. dom[w]=best next. */
    int *dom=(int*)malloc((size_t)voc.n*sizeof(int)); int *issrc=(int*)calloc((size_t)voc.n,sizeof(int));
    int *srctot=(int*)calloc((size_t)voc.n,sizeof(int));
    for(int w=0;w<voc.n && w<vcap;w++){ int tot=0,best=-1,bc=0;
        for(int i=0;i<cont[w].n;i++){ tot+=cont[w].cn[i]; if(cont[w].cn[i]>bc){bc=cont[w].cn[i]; best=cont[w].nx[i];} }
        dom[w]=best;
        if(best>=0 && tot>=min_count && (double)bc/tot>=min_share){ issrc[w]=1; srctot[w]=tot; }
    }
    /* keep only the top max_sources sources by total count */
    /* selection: repeatedly pick the highest-srctot source */
    int *keep=(int*)calloc((size_t)voc.n,sizeof(int)); int nkeep=0;
    for(int pass=0; pass<max_sources; pass++){ int b=-1;
        for(int w=0;w<voc.n;w++) if(issrc[w]&&!keep[w]&&(b<0||srctot[w]>srctot[b])) b=w;
        if(b<0) break; keep[b]=1; nkeep++;
    }
    rep->ms_mine = 1000.0*(clock()-t0)/CLOCKS_PER_SEC;

    /* 3) scoped vocab = kept sources + their targets; map to [0,V) */
    int *scoped=(int*)malloc((size_t)voc.n*sizeof(int)); for(int i=0;i<voc.n;i++) scoped[i]=-1;
    int V=0;
    for(int w=0;w<voc.n;w++) if(keep[w]){ if(scoped[w]<0) scoped[w]=V++; int d=dom[w]; if(scoped[d]<0) scoped[d]=V++; }
    if(V<2){ /* nothing to graduate */ free(dom);free(issrc);free(srctot);free(keep);free(scoped);
        for(int i=0;i<vcap;i++){free(cont[i].nx);free(cont[i].cn);} free(cont);
        for(int i=0;i<voc.n;i++) free(voc.w[i]); free(voc.w); return -1; }

    /* find a seed X (kept source) whose Y=dom(X) is also a kept source -> 2-step run */
    int seedw=-1;
    for(int w=0;w<voc.n;w++) if(keep[w] && keep[dom[w]]){ seedw=w; break; }

    /* 4) exemplar tables over scoped vocab: each kept source -> its dom (one-hot) */
    int E=0; for(int w=0;w<voc.n;w++) if(keep[w]) E++;
    double *Xt=(double*)calloc((size_t)E*V,sizeof(double));
    double *Yt=(double*)calloc((size_t)E*V,sizeof(double));
    { int r=0; for(int w=0;w<voc.n;w++) if(keep[w]){ Xt[(size_t)r*V+scoped[w]]=1.0; Yt[(size_t)r*V+scoped[dom[w]]]=1.0; r++; } }
    rep->exemplars=E; rep->vocab=V;

    /* 5) build + certify the two positionally tagged units (same map, distinct tags) */
    clock_t tb=clock();
    snprintf(gu->tag_a,sizeof(gu->tag_a),"w_a"); snprintf(gu->tag_b,sizeof(gu->tag_b),"w_b"); snprintf(gu->tag_c,sizeof(gu->tag_c),"w_c");
    int hid = V*2; if(hid<64) hid=64; if(hid>512) hid=512;
    int mxh = V*4; if(mxh<256) mxh=256; if(mxh>1024) mxh=1024;
    btn_init(&gu->btn_a,(size_t)V,(size_t)V,(size_t)hid,(size_t)mxh,0.08,1234u);
    btn_init(&gu->btn_b,(size_t)V,(size_t)V,(size_t)hid,(size_t)mxh,0.08,1234u);
    Port ia={PORT_ONEHOT,(size_t)V,1,""}; port_set_tag(&ia,gu->tag_a);
    Port ob={PORT_ONEHOT,(size_t)V,1,""}; port_set_tag(&ob,gu->tag_b);
    Port ib={PORT_ONEHOT,(size_t)V,1,""}; port_set_tag(&ib,gu->tag_b);
    Port oc={PORT_ONEHOT,(size_t)V,1,""}; port_set_tag(&oc,gu->tag_c);
    btn_set_ports(&gu->btn_a,ia,ob);
    btn_set_ports(&gu->btn_b,ib,oc);
    btn_train(&gu->btn_a,Xt,Yt,(size_t)E,6000);
    btn_train(&gu->btn_b,Xt,Yt,(size_t)E,6000);
    contract_init_borrowed(&gu->con_a,"step_ab",&gu->btn_a,Xt,Yt,(size_t)E);
    contract_init_borrowed(&gu->con_b,"step_bc",&gu->btn_b,Xt,Yt,(size_t)E);
    CertifyReport cra,crb; memset(&cra,0,sizeof(cra)); memset(&crb,0,sizeof(crb));
    rep->cert_a = btn_certify(&gu->btn_a,&gu->con_a,&cra);
    rep->cert_b = btn_certify(&gu->btn_b,&gu->con_b,&crb);
    gu->X=Xt; gu->Y=Yt; gu->vocab=V; gu->valid=1;
    rep->ms_build_certify = 1000.0*(clock()-tb)/CLOCKS_PER_SEC;

    /* 6) register certified */
    clock_t tr=clock();
    reg->require_certified=1;
    int ra=registry_add_certified(reg,&gu->btn_a,"step_ab",&gu->con_a);
    int rb=registry_add_certified(reg,&gu->btn_b,"step_bc",&gu->con_b);
    rep->units=(ra==0)+(rb==0);
    rep->ms_register = 1000.0*(clock()-tr)/CLOCKS_PER_SEC;

    /* 7) evict the seed run's tiles from fuzzy memory */
    clock_t te=clock();
    rep->mem_before = mem? tilemem_total(mem):0;
    if(seedw>=0){ snprintf(rep->seed,sizeof(rep->seed),"%s",voc.w[seedw]);
        snprintf(rep->mid,sizeof(rep->mid),"%s",voc.w[dom[seedw]]);
        snprintf(rep->dst,sizeof(rep->dst),"%s",voc.w[dom[dom[seedw]]]);
        gu->seed_idx=scoped[seedw]; gu->dst_idx=scoped[dom[dom[seedw]]];
        rep->seed_idx=gu->seed_idx; rep->dst_idx=gu->dst_idx;
        if(mem) rep->tiles_evicted = tilemem_evict_containing(mem, rep->seed);
        rep->ok = (rep->cert_a==0 && rep->cert_b==0 && rep->units==2) ? 1 : 0;
    }
    rep->mem_after = mem? tilemem_total(mem):0;
    rep->ms_evict = 1000.0*(clock()-te)/CLOCKS_PER_SEC;

    /* cleanup mining structures (BTNs/contracts/tables stay in gu) */
    free(dom);free(issrc);free(srctot);free(keep);free(scoped);
    for(int i=0;i<vcap;i++){ free(cont[i].nx); free(cont[i].cn); } free(cont);
    for(int i=0;i<voc.n;i++) free(voc.w[i]); free(voc.w);
    return rep->ok ? 0 : -1;
}
```

- [ ] **Step 3: It compiles only with a test (next task). Proceed to Task 3.**

---

## Task 3: `test_graduate.c` — full loop, router composition, benchmarks

**Files:**
- Create: `tests/test_graduate.c`
- Modify: `Makefile`

- [ ] **Step 1: Makefile**

Add vars near the corpus vars:
```make
GRADUATE_SRC := src/corpus/graduate.c
GRADUATE_TEST := tests/test_graduate.c
```
Add target (links nn + router + contract + corpus + pdf + tile_memory + graduate):
```make
graduate: $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CONSOLIDATE) $(SCAN) $(TILEMEM_SRC) $(GRADUATE_SRC) $(CORPUS_SRC) $(PDF_SRC) $(GRADUATE_TEST) include/corpus/graduate.h include/corpus/tile_memory.h include/router.h include/contract/contract.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(SRC) $(ROUTER) $(PLAN_TABLE) $(CONTRACT) $(CONSOLIDATE) $(SCAN) $(TILEMEM_SRC) $(GRADUATE_SRC) $(CORPUS_SRC) $(PDF_SRC) $(GRADUATE_TEST) $(LDFLAGS)
	./graduate
```
Add `graduate` to `.PHONY` (line 117) and to the `clean` rm list; add `tile_grad_ctl tile_grad_pdf` to the `rm -rf` line in `clean`.

- [ ] **Step 2: The test (controlled corpus loop + router composition + benchmarks)**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/corpus/graduate.h"
#include "../include/corpus/corpus_split.h"
#include "../include/corpus/tile_memory.h"
#include "../include/pdf/pdf_extract.h"
#include "../include/router.h"
#include "../include/nn.h"

static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)

static void reset_store(const char *dir){ char p[300];
    snprintf(p,sizeof(p),"%s/hot.bin",dir); remove(p);
    snprintf(p,sizeof(p),"%s/warm.bin",dir); remove(p); }

/* build a StrList of n copies of a deterministic run as sentences */
static void controlled_corpus(StrList *c, int n){
    strlist_init(c);
    for(int i=0;i<n;i++) strlist_push(c,"alpha beta gamma delta epsilon.");
}

static void print_bench(const GraduateReport *r){
    printf("\n[bench] %-22s %8s   %s\n","PHASE","TIME(ms)","METRICS");
    printf("[bench] %-22s %8.2f   vocab=%d exemplars=%d run=%s->%s->%s\n","mine",r->ms_mine,r->vocab,r->exemplars,r->seed,r->mid,r->dst);
    printf("[bench] %-22s %8.2f   2 units, certify a=%d b=%d (0=CERT)\n","distill+certify",r->ms_build_certify,r->cert_a,r->cert_b);
    printf("[bench] %-22s %8.2f   certified units registered=%d\n","register",r->ms_register,r->units);
    printf("[bench] %-22s %8.2f   tiles_evicted=%zu  mem %zu->%zu\n","evict",r->ms_evict,r->tiles_evicted,r->mem_before,r->mem_after);
}

static void test_controlled(void){
    StrList corpus; controlled_corpus(&corpus,8);
    reset_store("tile_grad_ctl");
    TileMemory *mem=tilemem_open("tile_grad_ctl",256,1000,0.99);
    for(size_t i=0;i<corpus.count;i++) tilemem_ingest(mem,corpus.lines[i],corpus.lines[i],"","ctl");
    size_t mem0=tilemem_total(mem);

    PrimitiveRegistry reg; registry_init(&reg);
    GraduatedUnits gu; GraduateReport rep;
    int rc=graduate_deterministic(&corpus,&reg,mem,2,0.9,32,&gu,&rep);
    print_bench(&rep);

    CHECK(rc==0,"graduation succeeded (2-step run found, both certified)");
    CHECK(rep.cert_a==0 && rep.cert_b==0,"both units btn_certify exactly");
    CHECK(rep.units==2,"two certified units registered");
    CHECK(rep.tiles_evicted>0 && rep.mem_after<mem0,"fuzzy memory shrank (knowledge graduated soft->hard)");

    /* router auto-composes the two certified units: w_a -> w_c */
    Port ia={PORT_ONEHOT,(size_t)gu.vocab,1,""}; port_set_tag(&ia,gu.tag_a);
    Port gc={PORT_ONEHOT,(size_t)gu.vocab,1,""}; port_set_tag(&gc,gu.tag_c);
    RoutePlan plan;
    int planned=route_plan(&reg,ia,gc,&plan);
    CHECK(planned==0,"router found a plan w_a -> w_c");
    printf("[route] discovered %zu-hop plan:", plan.length);
    for(size_t s=0;s<plan.length;s++) printf(" %s", plan.names[s]);
    printf("\n");
    CHECK(planned==0 && plan.length==2,"plan composes TWO certified units (non-monolithic)");

    double *in=(double*)calloc((size_t)gu.vocab,sizeof(double)); in[gu.seed_idx]=1.0;
    double *out=(double*)calloc((size_t)gu.vocab,sizeof(double));
    int exec=route_execute(&plan,in,(size_t)gu.vocab,out,(size_t)gu.vocab);
    int arg=0; for(int i=1;i<gu.vocab;i++) if(out[i]>out[arg]) arg=i;
    printf("[route] execute(%s) -> index %d (expected %d = '%s')\n", rep.seed, arg, gu.dst_idx, rep.dst);
    CHECK(exec==0,"route_execute ran the composed certified plan");
    CHECK(arg==gu.dst_idx,"composed units reconstruct the proven 2-step continuation X->Z");

    free(in); free(out);
    registry_free(&reg);
    graduated_units_free(&gu);
    tilemem_close(mem); strlist_free(&corpus);
}

static unsigned char *readfile(const char *p,size_t*len){ FILE*f=fopen(p,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0){fclose(f);return NULL;}
    unsigned char*b=malloc(sz); *len=fread(b,1,sz,f); fclose(f); return b; }

static void test_real_book(void){
    const char *path="C:/Users/Mokason/Downloads/aivalueplaybook.pdf";
    FILE *pr=fopen(path,"rb"); if(!pr){ printf("[realbook] not found - skipping\n"); return; } fclose(pr);
    size_t len=0; unsigned char *buf=readfile(path,&len);
    char *text=malloc(len*4+16); size_t tl=0;
    PdfStatus st=pdf_extract_text(buf,len,text,len*4+16,&tl);
    if(st!=PDF_OK){ printf("[realbook] extract failed\n"); free(text); free(buf); return; }
    StrList corpus; strlist_init(&corpus); corpus_split(text,&corpus);
    reset_store("tile_grad_pdf");
    TileMemory *mem=tilemem_open("tile_grad_pdf",256,500,0.5);
    for(size_t i=0;i<corpus.count;i++) tilemem_ingest(mem,corpus.lines[i],corpus.lines[i],"","book");

    PrimitiveRegistry reg; registry_init(&reg);
    GraduatedUnits gu; GraduateReport rep;
    int rc=graduate_deterministic(&corpus,&reg,mem,3,0.85,48,&gu,&rep);
    printf("\n[realbook] graduate rc=%d  run=%s->%s->%s  vocab=%d units=%d cert=(%d,%d) evicted=%zu mem %zu->%zu\n",
           rc, rep.seed, rep.mid, rep.dst, rep.vocab, rep.units, rep.cert_a, rep.cert_b,
           rep.tiles_evicted, rep.mem_before, rep.mem_after);
    print_bench(&rep);
    if(rc==0){
        Port ia={PORT_ONEHOT,(size_t)gu.vocab,1,""}; port_set_tag(&ia,gu.tag_a);
        Port gc={PORT_ONEHOT,(size_t)gu.vocab,1,""}; port_set_tag(&gc,gu.tag_c);
        RoutePlan plan;
        if(route_plan(&reg,ia,gc,&plan)==0){
            printf("[realbook] router composed %zu units:", plan.length);
            for(size_t s=0;s<plan.length;s++) printf(" %s",plan.names[s]); printf("\n");
            CHECK(plan.length>=2,"real-book graduation composed >=2 certified units");
        }
    } else {
        printf("[realbook] no clean 2-step deterministic run met the bar (honest: prose is noisy) - controlled corpus carries the composition proof\n");
    }
    if(gu.valid) graduated_units_free(&gu);
    registry_free(&reg);
    tilemem_close(mem); strlist_free(&corpus); free(text); free(buf);
}

int main(void){
    printf("=== test_graduate ===\n");
    test_controlled();
    test_real_book();
    printf("\n%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}
```

- [ ] **Step 3: Build and run (acceptance + benchmarks)**

Run: `make graduate; echo "exit=$?"`
Expected: controlled — `mine ... run=alpha->beta->gamma`, both `certify ... 0`, `register ... =2`, `evict tiles_evicted=8 mem 8->0`, `[route] discovered 2-hop plan: step_ab step_bc`, `execute(alpha) -> index N (expected N = 'gamma')`, all checks pass; benchmark table printed; real book reports what it mined; `N/N checks passed`, `exit=0`.
If a unit does NOT certify (`cert_a`/`cert_b` != 0): the V-way one-hot output needs crisper training — raise `btn_train` epochs (6000 → 12000) and/or `mxh`. (Same BTN-exact-certification lesson as the jsonstory escaper/unescaper.)

- [ ] **Step 4: Regression** — Run `make tiermem_test` and `make pdftest`; both still green (additive).

- [ ] **Step 5: Checkpoint (no git)** — re-run `make graduate`; confirm green.

---

## Self-Review (against the spec)

**Spec coverage:**
- §1.1 real `graduate_deterministic` operation → Task 2. ✓
- §1.2 router composes (plan length ≥2, require_certified) → Task 3 `route_plan`/`route_execute` asserts. ✓
- §1.3 knowledge soft→hard (memory shrinks, registry grows) → Task 1 evict + Task 2/3 (`mem_before>mem_after`, `units==2`). ✓
- §1.4 controlled (guaranteed) + real book (honest) → Task 3 `test_controlled` + `test_real_book`. ✓
- §1.5 benchmarks → Task 3 `print_bench` (per-phase ms + counts). ✓
- §3 mechanism (positional tags, dominant-next, scoped vocab) → Task 2. ✓
- §4 loop → Tasks 1–3. ✓
- §5 components (`graduate.c`, `tilemem_evict_containing`, `test_graduate.c`) → Tasks 1–3. ✓
- §8 limits respected (same map / 2 tags; short real runs honest; one slice). ✓

**Placeholder scan:** none — complete code + exact commands. The certify-tuning note is a contingency (the standard BTN-exact lesson), not a placeholder.

**Type consistency:** `GraduateReport`/`GraduatedUnits` fields used consistently; `graduate_deterministic`/`graduated_units_free`/`tilemem_evict_containing` signatures match headers; `route_plan(reg,in,goal,&plan)` + `route_execute(&plan,in,len,out,cap)` match `router.h`; `registry_add_certified(reg,btn,name,contract)` matches `contract.h`; tags `w_a/w_b/w_c` consistent across build + route.

---

## Execution note

Recommended: inline execution via `superpowers:executing-plans`. The real-book task auto-skips if the PDF is absent and degrades honestly if no clean 2-step run is mined; the controlled corpus carries the composition proof. If a unit fails exact certification, bump epochs/capacity per the Task-3 note.

---

## Implementation notes (executed inline — 2026-06-26)

`make graduate` = **9/9**, warning-clean; `make tiermem_test` 18/18 + `make pdftest` 19/19
(additive, zero core edits). The loop closes end to end.

**Benchmarks (printed by the run):**
- **Controlled** (`alpha beta gamma delta epsilon`): mine **0.0 ms** (vocab 6, 5 exemplars,
  run alpha→beta→gamma); distill+certify **41 ms** (2 units, both `cert=0`); register **0 ms**;
  evict **0 ms** (1 tile, **mem 1→0**); route → **2-hop `[step_ab, step_bc]`**;
  `execute(alpha) → gamma` ✓.
- **Real book** (`aivalueplaybook.pdf`): mine **398 ms** (vocab 74, 48 exemplars); distill+certify
  **7733 ms** (2 units, both `cert=0`) — the dominant cost (two V=74 BTN trains); register 1 ms;
  evict **0 ms** (**64 tiles, mem 6048→5984**); router **composed 2 units**.

Honest notes:
- The real book's best near-deterministic run is **noisy** (`ov→pg→a` = extraction-artifact
  tokens). The loop *mechanism* works on real data (mine→certify→register→evict→compose all
  fire), but the specific regularity is low-quality prose noise; the controlled corpus carries
  the clean composition proof.
- The two units compute the **same** dominant-next map with distinct positional tags
  (`w_a/w_b/w_c`) — the router composes by tag/type, so the tags encode chain order. The value
  is the *composition* (`X→Z`), proven and router-discovered.
- `distill+certify` dominates time (two BTN trains). Sharing one trained BTN across both tags
  would ~halve it; kept separate for clarity. One slice (2 units, 1 composition); the full
  DreamCoder-style engine is `library_evolve`.
- Warning cleanups: misleading-indentation splits + an empty-corpus guard that also silenced a
  spurious `-Walloc-size` range warning. Purely additive: new `graduate.c`/`graduate.h`,
  `test_graduate.c`, a `tilemem_evict_containing` addition, the `graduate` target.
