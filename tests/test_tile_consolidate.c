#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/corpus/tile_memory.h"
#include "../include/corpus/corpus_split.h"
#include "../include/pdf/pdf_extract.h"

static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)
static void reset_store(const char *dir){ char p[300];
    snprintf(p,sizeof(p),"%s/hot.bin",dir); remove(p);
    snprintf(p,sizeof(p),"%s/warm.bin",dir); remove(p);
    snprintf(p,sizeof(p),"%s/idf.bin",dir); remove(p);
    snprintf(p,sizeof(p),"%s/synonyms.bin",dir); remove(p); }

static void test_consolidate_unit(void){
    reset_store("cons_ctl");
    TileMemory *m=tilemem_open("cons_ctl",256,1000,0.99);
    /* churn~attrition become PPMI neighbors via the two cooc tiles (fillers are df=1 -> no
       neighbors). P1/P2 are a churn/attrition paraphrase. subA/subB exercise the min-coverage
       subset guard (subB's extra unique terms drag coverage(subB->subA) below tau). D1/D2 are
       unrelated and must survive. */
    const char *docs[]={
        "churn attrition foo bar",          /* cooc1 */
        "churn attrition baz qux",          /* cooc2 */
        "churn rises sharply",              /* P1 */
        "attrition rises sharply",          /* P2  (paraphrase of P1) */
        "weather sunny today",              /* D1 */
        "market crashed hard",              /* D2 */
        "alpha beta gamma",                 /* subA */
        "alpha beta gamma delta epsilon zeta eta theta" /* subB (superset of subA) */
    };
    for(int i=0;i<8;i++) tilemem_ingest(m,docs[i],docs[i],"","c");
    CHECK(tilemem_total(m)==8,"8 tiles ingested (no lexical dedup collisions)");

    ConsolidateReport rep;
    size_t merges=tilemem_consolidate(m,0.7,3,1.0,64,&rep);
    CHECK(merges==1,"exactly one merge (the churn/attrition paraphrase)");
    CHECK(rep.tiles_before==8 && rep.tiles_after==7,"report: 8 -> 7 tiles");
    CHECK(tilemem_total(m)==7,"one tile merged away");

    TileHit h[8];
    CHECK(tilemem_search(m,"rises sharply",8,h,8)==1,"P1/P2 collapsed to a single tile");
    CHECK(tilemem_search(m,"weather sunny",8,h,8)>=1,"unrelated D1 survived");
    CHECK(tilemem_search(m,"market crashed",8,h,8)>=1,"unrelated D2 survived");
    CHECK(tilemem_search(m,"alpha beta gamma",8,h,8)==2,"subset pair NOT merged (min-coverage guard)");
    tilemem_close(m);
}

static unsigned char *readfile(const char *p,size_t*len){ FILE*f=fopen(p,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0){fclose(f);return NULL;}
    unsigned char*b=malloc((size_t)sz); *len=fread(b,1,(size_t)sz,f); fclose(f); return b; }

static void test_book_report(void){
    const char *path="C:/Users/Mokason/Downloads/aivalueplaybook.pdf";
    FILE *pr=fopen(path,"rb"); if(!pr){ printf("[book] not found - skipping report\n"); return; } fclose(pr);
    size_t len=0; unsigned char *buf=readfile(path,&len); if(!buf) return;
    char *text=malloc(len*4+16); size_t tl=0;
    if(pdf_extract_text(buf,len,text,len*4+16,&tl)!=PDF_OK){ free(text); free(buf); return; }
    StrList s; strlist_init(&s); corpus_split(text,&s);
    reset_store("cons_book");
    TileMemory *m=tilemem_open("cons_book",256,9000,0.6);   /* large hot_cap: tiles resident for consolidation */
    for(size_t i=0;i<s.count;i++) if(corpus_quality_keep(s.lines[i])) tilemem_ingest(m,s.lines[i],s.lines[i],"","book");
    ConsolidateReport rep;
    size_t merges=tilemem_consolidate(m,0.7,3,0.1,64,&rep);
    printf("\n[bench] consolidate %.0f ms ; tiles %zu -> %zu ; merges=%zu ; comparisons=%zu\n",
           rep.ms, rep.tiles_before, rep.tiles_after, merges, rep.comparisons);
    /* sanity: never increases the tile count; at most removes one per merge */
    CHECK(rep.tiles_after<=rep.tiles_before,"consolidation never grows the store");
    CHECK(rep.tiles_before-rep.tiles_after==merges,"each merge removes exactly one tile");
    tilemem_close(m);
    free(text); free(buf); strlist_free(&s);
}

static void test_prune_candidates(void){
    reset_store("cons_prune");
    TileMemory *m=tilemem_open("cons_prune",256,1000,0.99);
    /* 10 tiles all sharing the common term "hub"; the other two terms of each are unique
       (df=1). No paraphrases (coverage between any two = just hub = 1/3 < tau). With pruning,
       "hub" (df=10) is skipped in gathering, so no candidates are examined. */
    const char *docs[]={
        "hub fa1 fa2","hub fb1 fb2","hub fc1 fc2","hub fd1 fd2","hub fe1 fe2",
        "hub ff1 ff2","hub fg1 fg2","hub fh1 fh2","hub fi1 fi2","hub fj1 fj2"};
    for(int i=0;i<10;i++) tilemem_ingest(m,docs[i],docs[i],"","c");

    ConsolidateReport r_off, r_on;
    size_t m_off=tilemem_consolidate(m,0.7,3,1.0,0,&r_off);   /* pruning off, cap off */
    size_t m_on =tilemem_consolidate(m,0.7,3,0.3,0,&r_on);    /* prune df>3 -> "hub" skipped */
    CHECK(m_off==0 && m_on==0,"hub-only tiles never merge (coverage below tau both ways)");
    CHECK(r_off.comparisons>0,"without pruning, hub links every pair -> work done");
    CHECK(r_on.comparisons==0,"with pruning, the common term is skipped -> no pairs examined");
    CHECK(r_on.comparisons<r_off.comparisons,"pruning strictly reduces candidate work");
    tilemem_close(m);
}

static void test_cap_candidates(void){
    reset_store("cons_cap");
    TileMemory *m=tilemem_open("cons_cap",256,1000,0.99);
    /* 10 tiles sharing "hub" (df=10) + unique df=1 fillers: not paraphrases. With no df-prefilter
       (max_df_frac=1.0), every tile gathers the other 9 via "hub"; the cap bounds that. */
    const char *docs[]={
        "hub fa1 fa2","hub fb1 fb2","hub fc1 fc2","hub fd1 fd2","hub fe1 fe2",
        "hub ff1 ff2","hub fg1 fg2","hub fh1 fh2","hub fi1 fi2","hub fj1 fj2"};
    for(int i=0;i<10;i++) tilemem_ingest(m,docs[i],docs[i],"","c");
    ConsolidateReport r0, r1;
    size_t m0=tilemem_consolidate(m,0.7,3,1.0,0,&r0);   /* cap off (unbounded) */
    size_t m1=tilemem_consolidate(m,0.7,3,1.0,3,&r1);   /* cap = 3 per tile */
    CHECK(m0==0 && m1==0,"hub-only tiles never merge (coverage below tau)");
    CHECK(r1.comparisons<=30,"cap bounds work to <= n*max_cand (10*3)");
    CHECK(r1.comparisons<r0.comparisons,"cap strictly reduces work vs unbounded");
    tilemem_close(m);
}

int run_test_tile_consolidate(void){
    printf("=== test_consolidate ===\n");
    test_consolidate_unit();
    test_prune_candidates();
    test_cap_candidates();
    test_book_report();
    printf("\n%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}

#ifndef TEST_ALL
int main(void) { return run_test_tile_consolidate() == 0 ? 0 : 1; }
#endif
