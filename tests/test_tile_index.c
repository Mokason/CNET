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

/* assert tilemem_search == tilemem_search_linear for one query (ids, scores, order).
   Both searchers share m->res (the value arena), and the second call frees it on entry,
   so we strdup the first call's value strings before the second invalidates them.
   strdup handles arbitrary-length values (book sentences can exceed 256 chars). */
static int same_results(TileMemory *m, const char *q){
    TileHit a[16], b[16];
    int na=tilemem_search(m,q,16,a,16);
    char *av[16];
    for(int i=0;i<na && i<16;i++) av[i]=strdup(a[i].value?a[i].value:"");
    int nb=tilemem_search_linear(m,q,16,b,16);
    int ok=1;
    if(na!=nb){ ok=0; }
    else{
        for(int i=0;i<na;i++){
            if(a[i].score!=b[i].score){ ok=0; break; }
            if(strcmp(a[i].id,b[i].id)!=0){ ok=0; break; }
            if(strcmp(av[i],b[i].value?b[i].value:"")!=0){ ok=0; break; }
        }
    }
    for(int i=0;i<na && i<16;i++) free(av[i]);
    return ok;
}

static void test_identity_small(void){
    reset_store("tix_small");
    TileMemory *m=tilemem_open("tix_small",256,1000,0.99);
    const char *docs[]={"alpha beta gamma","beta delta","gamma epsilon zeta",
                        "alpha epsilon","delta zeta eta","theta iota"};
    for(int i=0;i<6;i++) tilemem_ingest(m,docs[i],docs[i],"","c");
    const char *qs[]={"alpha","beta gamma","epsilon zeta","nomatch","alpha beta delta eta"};
    for(int i=0;i<5;i++) CHECK(same_results(m,qs[i]),"index == linear (small, all HOT)");
    tilemem_close(m);
}

static void test_identity_warm(void){
    reset_store("tix_warm");
    TileMemory *m=tilemem_open("tix_warm",256,4,0.99);   /* hot_cap=4 forces spill to WARM */
    const char *docs[]={"alpha beta","gamma delta","alpha gamma","beta epsilon",
                        "delta zeta","alpha epsilon zeta","gamma eta","beta delta theta"};
    for(int i=0;i<8;i++) tilemem_ingest(m,docs[i],docs[i],"","c");   /* >4 -> WARM populated */
    const char *qs[]={"alpha","beta delta","gamma eta","zeta","nomatch","alpha beta gamma delta"};
    for(int i=0;i<6;i++) CHECK(same_results(m,qs[i]),"index == linear (forced WARM)");
    /* after an eviction, identity still holds */
    tilemem_evict_containing(m,"alpha");
    for(int i=0;i<6;i++) CHECK(same_results(m,qs[i]),"index == linear (after evict)");
    tilemem_close(m);
}

static void test_identity_expansion(void){
    reset_store("tix_exp");
    TileMemory *m=tilemem_open("tix_exp",256,3,0.99);
    const char *docs[]={"alpha xray one","alpha xray two","beta yankee three",
                        "beta yankee four","xray yankee five","delta echo six"};
    for(int i=0;i<6;i++) tilemem_ingest(m,docs[i],docs[i],"","c");
    tilemem_set_expansion(m,0.35,5,2,1,0.9,64);   /* PPMI soft terms participate in candidates */
    const char *qs[]={"alpha beta","xray","delta","beta yankee"};
    for(int i=0;i<4;i++) CHECK(same_results(m,qs[i]),"index == linear (with PPMI expansion)");
    tilemem_close(m);
}

static unsigned char *readfile(const char *p, size_t *len){
    FILE *f=fopen(p,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0){ fclose(f); return NULL; }
    unsigned char *b=malloc((size_t)sz); *len=fread(b,1,(size_t)sz,f); fclose(f); return b;
}

static double bench_ms(TileMemory *m, const char *q, int linear, int reps){
    TileHit h[8]; clock_t t0=clock();
    for(int r=0;r<reps;r++){
        if(linear) tilemem_search_linear(m,q,8,h,8);
        else tilemem_search(m,q,8,h,8);
    }
    return 1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC/(double)reps;
}

static void test_book_bench(void){
    const char *path="C:/Users/Mokason/Downloads/aivalueplaybook.pdf";
    FILE *pr=fopen(path,"rb");
    if(!pr){ printf("[book] not found - skipping benchmark\n"); return; }
    fclose(pr);
    size_t len=0; unsigned char *buf=readfile(path,&len); if(!buf) return;
    char *text=malloc(len*4+16); size_t tl=0;
    if(pdf_extract_text(buf,len,text,len*4+16,&tl)!=PDF_OK){ free(text); free(buf); return; }
    StrList s; strlist_init(&s); corpus_split(text,&s);
    reset_store("tix_book");
    TileMemory *m=tilemem_open("tix_book",256,500,0.6);   /* small HOT -> most tiles spill to WARM */
    for(size_t i=0;i<s.count;i++){
        if(corpus_quality_keep(s.lines[i])) tilemem_ingest(m,s.lines[i],s.lines[i],"","book");
    }
    printf("\n[bench] tiles=%zu  hot=%zu  warm=%zu\n",
           tilemem_total(m), tilemem_hot_count(m), tilemem_warm_count(m));
    const char *qs[]={"last mile problem","storytelling narrative","the and of"};
    for(int i=0;i<3;i++){
        double lin=bench_ms(m,qs[i],1,20), idx=bench_ms(m,qs[i],0,20);
        CHECK(same_results(m,qs[i]),"book: index == linear");
        printf("[bench] q=\"%-22s\" linear %.3f ms  index %.3f ms  (%.1fx)\n",
               qs[i], lin, idx, lin/(idx>0?idx:1e-9));
    }
    tilemem_close(m);
    free(text); free(buf); strlist_free(&s);
}

int run_test_tileindex(void){
    printf("=== test_tile_index ===\n");
    test_identity_small();
    test_identity_warm();
    test_identity_expansion();
    test_book_bench();
    printf("\n%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}

#ifndef TEST_ALL
int main(void) { return run_test_tileindex() == 0 ? 0 : 1; }
#endif
