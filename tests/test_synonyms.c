#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/corpus/synonyms.h"
#include "../include/corpus/tile_memory.h"
#include "../include/corpus/corpus_split.h"
#include "../include/pdf/pdf_extract.h"
#include <time.h>

static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)

/* in-test document-frequency source */
typedef struct { const char *t; unsigned df; } DF;
static DF g_df[64]; static int g_ndf=0;
static void df_set(const char *t, unsigned df){ g_df[g_ndf].t=t; g_df[g_ndf].df=df; g_ndf++; }
static unsigned df_lookup(void *ctx,const char*term){ (void)ctx;
    for(int i=0;i<g_ndf;i++) if(!strcmp(g_df[i].t,term)) return g_df[i].df;
    return 0; }

static float nbr_weight(Synonyms *s, const char *term, const char *want){
    const char *nb[8]; float pp[8]; int n=syn_neighbors(s,term,nb,pp,8);
    for(int i=0;i<n;i++) if(!strcmp(nb[i],want)) return pp[i];
    return -1.0f;
}

static void test_ppmi_math(void){
    /* 4 tiles: {a,b,x,h?}, {a,b,x}, {c,d,x}, {c,d,x}. x in all (hub), h once (hapax). */
    g_ndf=0;
    df_set("a",2); df_set("b",2); df_set("c",2); df_set("d",2); df_set("x",4); df_set("h",1);
    Synonyms *s=syn_new();
    const char *t1[]={"a","b","x","h"}; syn_observe_tile(s,t1,4);
    const char *t2[]={"a","b","x"};     syn_observe_tile(s,t2,3);
    const char *t3[]={"c","d","x"};     syn_observe_tile(s,t3,3);
    const char *t4[]={"c","d","x"};     syn_observe_tile(s,t4,3);
    /* N=4, k=5, min_cooc=2, df_frac=0.5 (x df4 > 2 -> excluded), min_df=2 (h df1 -> excluded) */
    syn_finalize(s, df_lookup, NULL, 4, 5, 2, 0.5, 2, 0);

    /* PMI(a,b)=log(c*N/(df_a*df_b))=log(2*4/(2*2))=log(2)=0.6931 */
    float w=nbr_weight(s,"a","b");
    CHECK(w>0.0f,"a has neighbor b");
    CHECK(fabsf(w-0.6931f)<0.01f,"PPMI(a,b)=ln(2) as hand-computed");
    CHECK(nbr_weight(s,"b","a")>0.0f,"twins are mutual (b<->a)");
    CHECK(nbr_weight(s,"a","x")<0.0f,"hub term x excluded by df_frac (not a neighbor)");
    CHECK(nbr_weight(s,"a","h")<0.0f,"hapax term h excluded by min_df (not a neighbor)");
    CHECK(nbr_weight(s,"a","c")<0.0f,"a and c never co-occur -> no edge");
    syn_free(s);
}

static void test_roundtrip(void){
    g_ndf=0;
    df_set("a",2); df_set("b",2); df_set("c",2); df_set("d",2);
    Synonyms *s=syn_new();
    const char *t1[]={"a","b"}; syn_observe_tile(s,t1,2);
    const char *t2[]={"a","b"}; syn_observe_tile(s,t2,2);
    const char *t3[]={"c","d"}; syn_observe_tile(s,t3,2);
    const char *t4[]={"c","d"}; syn_observe_tile(s,t4,2);
    syn_finalize(s, df_lookup, NULL, 4, 5, 2, 1.0, 1, 0);
    float before=nbr_weight(s,"a","b");
    CHECK(before>0.0f,"pre-save: a->b edge exists");
    CHECK(syn_save(s,"syn_rt.txt",4)==0,"save ok");
    syn_free(s);

    Synonyms *s2=syn_new(); size_t stamp=0;
    CHECK(syn_load(s2,"syn_rt.txt",&stamp)==0,"load ok");
    CHECK(stamp==4,"stamp round-trips");
    float after=nbr_weight(s2,"a","b");
    CHECK(after>0.0f,"post-load: a->b edge survives");
    CHECK(fabsf(after-before)<0.001f,"weight round-trips exactly");
    syn_free(s2);
    remove("syn_rt.txt");
}

static void reset_syn_store(const char *dir){ char p[300];
    snprintf(p,sizeof(p),"%s/hot.bin",dir);      remove(p);
    snprintf(p,sizeof(p),"%s/warm.bin",dir);     remove(p);
    snprintf(p,sizeof(p),"%s/idf.bin",dir);      remove(p);
    snprintf(p,sizeof(p),"%s/synonyms.bin",dir); remove(p); }

static int hits_contain(TileHit *h, int n, const char *needle){
    for(int i=0;i<n;i++) if(h[i].value && strstr(h[i].value,needle)) return 1;
    return 0;
}

static void test_gap_closer(void){
    reset_syn_store("syn_ctl");
    TileMemory *m=tilemem_open("syn_ctl",256,1000,0.99);
    /* alpha co-occurs with xray; beta co-occurs with yankee. The answer tile shares
       NO words with the query "alpha beta" but contains xray + yankee. */
    tilemem_ingest(m,"alpha xray one","alpha xray one","","c");
    tilemem_ingest(m,"alpha xray two","alpha xray two","","c");
    tilemem_ingest(m,"beta yankee three","beta yankee three","","c");
    tilemem_ingest(m,"beta yankee four","beta yankee four","","c");
    tilemem_ingest(m,"xray yankee zzanswer","xray yankee zzanswer","","c");
    tilemem_ingest(m,"delta echo six","delta echo six","","c");
    tilemem_ingest(m,"foxtrot golf seven","foxtrot golf seven","","c");

    /* expansion OFF (alpha=0): the answer tile is missed (no shared words). */
    TileHit h[10]; int n=tilemem_search(m,"alpha beta",10,h,10);
    CHECK(n>0,"baseline returns the alpha/beta tiles");
    CHECK(!hits_contain(h,n,"zzanswer"),"baseline MISSES the synonym-only answer tile");

    /* expansion ON: alpha->xray, beta->yankee surfaces the answer tile. */
    tilemem_set_expansion(m, 0.35, 5, 2, 1, 0.9, 64);
    n=tilemem_search(m,"alpha beta",10,h,10);
    CHECK(hits_contain(h,n,"zzanswer"),"expansion FINDS the zero-overlap answer tile (gap closed)");

    /* normal lexical retrieval still works with expansion on. */
    n=tilemem_search(m,"delta",10,h,10);
    CHECK(n>0 && hits_contain(h,n,"delta echo"),"expansion-on still returns direct lexical hit");
    tilemem_close(m);
}

static void test_alpha0_reversible(void){
    reset_syn_store("syn_rev");
    TileMemory *m=tilemem_open("syn_rev",256,1000,0.99);
    tilemem_ingest(m,"alpha xray one","alpha xray one","","c");
    tilemem_ingest(m,"alpha xray two","alpha xray two","","c");
    tilemem_ingest(m,"xray yankee zzanswer","xray yankee zzanswer","","c");
    /* explicitly enable then disable: alpha=0 must behave exactly like never-enabled. */
    tilemem_set_expansion(m, 0.0, 5, 2, 1, 0.9, 64);
    TileHit h[10]; int n=tilemem_search(m,"alpha beta",10,h,10);
    CHECK(!hits_contain(h,n,"zzanswer"),"alpha=0 reproduces baseline (no expansion)");
    tilemem_close(m);
}

static unsigned char *readfile(const char *p,size_t*len){ FILE*f=fopen(p,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0){fclose(f);return NULL;}
    unsigned char*b=malloc((size_t)sz); *len=fread(b,1,(size_t)sz,f); fclose(f); return b; }

static void test_book_bench(void){
    const char *path="C:/Users/Mokason/Downloads/aivalueplaybook.pdf";
    FILE *pr=fopen(path,"rb"); if(!pr){ printf("[book] not found - skipping benchmark\n"); return; } fclose(pr);
    size_t len=0; unsigned char *buf=readfile(path,&len); if(!buf) return;
    char *text=malloc(len*4+16); size_t tl=0;
    if(pdf_extract_text(buf,len,text,len*4+16,&tl)!=PDF_OK){ free(text); free(buf); return; }
    StrList s; strlist_init(&s); corpus_split(text,&s);
    reset_syn_store("syn_book");
    TileMemory *m=tilemem_open("syn_book",256,9000,0.6);
    for(size_t i=0;i<s.count;i++) if(corpus_quality_keep(s.lines[i]))
        tilemem_ingest(m,s.lines[i],s.lines[i],"","book");
    tilemem_set_expansion(m, 0.35, 5, 2, 2, 0.5, 64);
    const char *seeds[]={"story","data","mile","customer"};

    /* A: discount OFF (raw PPMI) */
    tilemem_set_discount(m, 0);
    clock_t t0=clock(); tilemem_build_synonyms(m);
    double build0=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;
    const Synonyms *soff=tilemem_synonyms(m);
    printf("\n[bench] build(discount off) %.0f ms over %zu tiles ; vocab=%zu ; edges=%zu\n",
           build0, tilemem_total(m), syn_term_count(soff), syn_neighbor_edges(soff));
    for(int q=0;q<4;q++){ const char *nb[5]; float pp[5]; int got=syn_neighbors(soff,seeds[q],nb,pp,5);
        printf("[neighbors off] %-9s ->", seeds[q]);
        for(int i=0;i<got;i++) printf(" %s(%.2f)", nb[i], pp[i]);
        printf("%s\n", got? "":" (none)"); }

    /* B: discount ON (support-weighted) */
    tilemem_set_discount(m, 1);
    t0=clock(); tilemem_build_synonyms(m);
    double build1=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;
    const Synonyms *son=tilemem_synonyms(m);
    size_t terms=syn_term_count(son), edges=syn_neighbor_edges(son);
    printf("[bench] build(discount on)  %.0f ms ; vocab=%zu ; edges=%zu\n", build1, terms, edges);
    for(int q=0;q<4;q++){ const char *nb[5]; float pp[5]; int got=syn_neighbors(son,seeds[q],nb,pp,5);
        printf("[neighbors on ] %-9s ->", seeds[q]);
        for(int i=0;i<got;i++) printf(" %s(%.2f)", nb[i], pp[i]);
        printf("%s\n", got? "":" (none)"); }

    /* per-query overhead (discount on): baseline (alpha=0) vs expanded (alpha=0.35) */
    TileHit h[5];
    tilemem_set_expansion(m, 0.0, 5, 2, 2, 0.5, 64);
    t0=clock(); int nb0=tilemem_search(m,"storytelling narrative",5,h,5);
    double q0=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;
    tilemem_set_expansion(m, 0.35, 5, 2, 2, 0.5, 64);
    t0=clock(); int nb1=tilemem_search(m,"storytelling narrative",5,h,5);
    double q1=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;
    printf("[bench] query baseline(alpha=0) %.3f ms (%d hits) ; expanded(alpha=0.35) %.3f ms (%d hits)\n",
           q0, nb0, q1, nb1);

    CHECK(terms>0,"book produced a non-empty synonym vocabulary");
    CHECK(edges>0,"book produced at least one PPMI neighbor edge");
    tilemem_close(m);
    free(text); free(buf); strlist_free(&s);
}

static void test_discount(void){
    /* (a,b) co-occur 4x with large marginals; (a,r) co-occur 2x with r rare.
       Chosen so raw PMI(a,b) == PMI(a,r) == log(6.25):
       4*100/(8*8) = 2*100/(8*4) = 6.25. Support-discounting must break that tie. */
    g_ndf=0; df_set("a",8); df_set("b",8); df_set("r",4);
    const char *ab[]={"a","b"}; const char *ar[]={"a","r"};

    /* discount OFF: equal raw PMI -> equal weights == log(6.25) */
    Synonyms *s0=syn_new();
    for(int i=0;i<4;i++) syn_observe_tile(s0,ab,2);
    for(int i=0;i<2;i++) syn_observe_tile(s0,ar,2);
    syn_finalize(s0, df_lookup, NULL, 100, 5, 2, 1.0, 1, 0);
    float wb0=nbr_weight(s0,"a","b"), wr0=nbr_weight(s0,"a","r");
    CHECK(wb0>0.0f && wr0>0.0f,"discount-off: both neighbors present");
    CHECK(fabsf(wb0-wr0)<0.01f,"discount-off: equal raw PMI -> equal weights");
    CHECK(fabsf(wb0-1.8326f)<0.01f,"discount-off: weight == raw PMI log(6.25)");
    syn_free(s0);

    /* discount ON: support factor reorders + shrinks the rare edge */
    Synonyms *s1=syn_new();
    for(int i=0;i<4;i++) syn_observe_tile(s1,ab,2);
    for(int i=0;i<2;i++) syn_observe_tile(s1,ar,2);
    syn_finalize(s1, df_lookup, NULL, 100, 5, 2, 1.0, 1, 1);
    float wb1=nbr_weight(s1,"a","b"), wr1=nbr_weight(s1,"a","r");
    CHECK(wb1>wr1,"discount-on: well-supported (a,b) outranks rare (a,r)");
    CHECK(wr1<wr0,"discount-on: rare edge (a,r) is strictly shrunk vs raw");
    CHECK(wb1<wb0,"discount-on: even the supported edge is scaled below raw PMI");
    syn_free(s1);
}

int run_test_synonyms(void){
    printf("=== test_synonyms ===\n");
    test_ppmi_math();
    test_roundtrip();
    test_discount();
    test_gap_closer();
    test_alpha0_reversible();
    test_book_bench();
    printf("\n%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}

#ifndef TEST_ALL
int main(void) { return run_test_synonyms() == 0 ? 0 : 1; }
#endif
