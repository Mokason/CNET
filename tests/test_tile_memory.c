#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/corpus/tile_memory.h"
#include "../include/pdf/pdf_extract.h"
#include "../include/corpus/corpus_split.h"

static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)

/* portable clean reset of a store dir's contents */
static void reset_store(const char *dir){
    char p[300];
    snprintf(p,sizeof(p),"%s/hot.bin",dir);  remove(p);
    snprintf(p,sizeof(p),"%s/warm.bin",dir); remove(p);
}

static const char *BOOK_A[]={
    "the team aligns on the goal and the value of the work",
    "data and the kind of teams you need to build the system",
    "change is the focus and the value of the investment grows",
    "the leader shares the story and builds trust with the team" };
static const char *BOOK_B[]={
    "the team aligns on the goal and the value of the work",          /* verbatim */
    "change is the focus and the value of the investment grows",      /* verbatim */
    "the team builds the system and shares the value of the data",    /* paraphrase, shared vocab */
    "the leader aligns the goal and grows the trust of the team" };   /* paraphrase, shared vocab */
#define NB 4

static int ingest_book(TileMemory *m, const char **bk, int n){ int reused=0;
    for(int i=0;i<n;i++) reused += (tilemem_ingest(m,bk[i],bk[i],"","A")==0);
    return reused;
}

static void test_open(void){
    reset_store("tile_store_test");
    TileMemory *m=tilemem_open("tile_store_test",256,1000,0.5);
    CHECK(m!=NULL,"open store");
    CHECK(m && tilemem_total(m)==0,"new store empty");
    tilemem_close(m);
}

static void test_dedup(void){
    reset_store("tile_store_test");
    TileMemory *m=tilemem_open("tile_store_test",256,1000,0.99);
    tilemem_ingest(m,BOOK_A[0],BOOK_A[0],"","A");
    int r=tilemem_ingest(m,BOOK_A[0],BOOK_A[0],"","A");
    CHECK(r==0,"identical sentence is fuzzy-deduped (reused)");
    CHECK(tilemem_total(m)==1,"identical ingest keeps 1 tile");
    tilemem_close(m);
}

static void test_fuzzy_vs_exact(void){
    reset_store("tile_store_test");
    TileMemory *me=tilemem_open("tile_store_test",256,1000,0.999);
    ingest_book(me,BOOK_A,NB); int reuse_exact=ingest_book(me,BOOK_B,NB);
    tilemem_close(me);
    reset_store("tile_store_test");
    TileMemory *mf=tilemem_open("tile_store_test",256,1000,0.5);
    ingest_book(mf,BOOK_A,NB); int reuse_fuzzy=ingest_book(mf,BOOK_B,NB);
    tilemem_close(mf);
    printf("[fuzzy] reuse exact=%d fuzzy=%d (of %d)\n", reuse_exact, reuse_fuzzy, NB);
    CHECK(reuse_exact>=2,"exact reuse catches the 2 verbatim sentences");
    CHECK(reuse_fuzzy>reuse_exact,"fuzzy reuse exceeds exact (AICIMO upgrade)");
}

static void test_search(void){
    reset_store("tile_store_test");
    TileMemory *m=tilemem_open("tile_store_test",256,1000,0.5);
    ingest_book(m,BOOK_A,NB);
    TileHit hits[4]; int n=tilemem_search(m,"what is the value of the team goal",3,hits,4);
    CHECK(n>0,"search returns hits");
    CHECK(n>0 && hits[0].score>0,"top hit has positive score");
    tilemem_close(m);
}

static void test_residency(void){
    reset_store("tile_store_res");
    TileMemory *m=tilemem_open("tile_store_res",256,3,0.99);   /* tiny HOT cap = 3 */
    const char *S[]={"alpha one two three","beta four five six","gamma seven eight nine",
                     "delta ten eleven twelve","epsilon thirteen fourteen fifteen"};
    for(int i=0;i<5;i++) tilemem_ingest(m,S[i],S[i],"","src");
    CHECK(tilemem_hot_count(m)<=3,"HOT stays within cap (bounded RAM)");
    CHECK(tilemem_total(m)==5,"total grows past cap (capacity on disk)");
    CHECK(tilemem_warm_count(m)>=2,"overflow spilled to WARM");
    tilemem_close(m);
}

static void test_persist(void){
    reset_store("tile_store_per");
    TileMemory *m=tilemem_open("tile_store_per",256,1000,0.5);
    ingest_book(m,BOOK_A,NB); size_t total=tilemem_total(m);
    tilemem_close(m);
    TileMemory *m2=tilemem_open("tile_store_per",256,1000,0.5);
    CHECK(tilemem_total(m2)==total,"store persists across open/close");
    int reused=ingest_book(m2,BOOK_A,NB);
    CHECK(reused==NB,"re-ingesting prior content is fully deduped (durable + idempotent)");
    tilemem_close(m2);
}

static void test_certifiable(void){
    reset_store("tile_store_cert");
    TileMemory *m=tilemem_open("tile_store_cert",256,1000,0.5);
    for(int i=0;i<5;i++) tilemem_ingest(m,BOOK_A[0],BOOK_A[0],"","A");
    CHECK(tilemem_certifiable(m,3,0.95)>=1,"a high-count stable tile is a contract candidate");
    tilemem_close(m);
}

static unsigned char *readfile(const char *p,size_t*len){ FILE*f=fopen(p,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0){fclose(f);return NULL;}
    unsigned char*b=malloc(sz); *len=fread(b,1,sz,f); fclose(f); return b; }

static void test_real_pdf(void){
    const char *path="C:/Users/Mokason/Downloads/aivalueplaybook.pdf";
    FILE *pr=fopen(path,"rb"); if(!pr){ printf("[realpdf] not found - skipping\n"); return; } fclose(pr);
    reset_store("tile_mem_pdf");
    TileMemory *m=tilemem_open("tile_mem_pdf",256,500,0.5);   /* HOT cap 500 vs ~6900 sentences */
    size_t len=0; unsigned char *buf=readfile(path,&len);
    char *text=malloc(len*4+16); size_t tl=0;
    PdfStatus st=pdf_extract_text(buf,len,text,len*4+16,&tl);
    int reused=0, total_in=0;
    if(st==PDF_OK){ StrList s; strlist_init(&s); corpus_split(text,&s);
        for(size_t i=0;i<s.count;i++){ total_in++; reused += (tilemem_ingest(m,s.lines[i],s.lines[i],"","aivalueplaybook")==0); }
        strlist_free(&s); }
    printf("[realpdf] ingested=%d reused=%d ; HOT=%zu WARM=%zu total=%zu\n",
           total_in, reused, tilemem_hot_count(m), tilemem_warm_count(m), tilemem_total(m));
    CHECK(st==PDF_OK,"real PDF extracted");
    CHECK(tilemem_hot_count(m)<=500,"HOT stayed within cap on a whole book (bounded RAM)");
    CHECK(tilemem_total(m)>1000,"memory grew well past HOT cap (unbounded on disk)");
    TileHit hits[5]; int n=tilemem_search(m,"value of data and teams",5,hits,5);
    printf("[realpdf] recall top-%d for 'value of data and teams':\n",n);
    for(int i=0;i<n;i++) printf("    %.3f  %.70s\n", hits[i].score, hits[i].value?hits[i].value:"");
    CHECK(n>0,"recall returns hits from the persisted book memory");
    free(text); free(buf); tilemem_close(m);
}

int run_test_tiermem(void){
    printf("=== test_tile_memory ===\n");
    test_open();
    test_dedup();
    test_fuzzy_vs_exact();
    test_search();
    test_residency();
    test_persist();
    test_certifiable();
    test_real_pdf();
    printf("%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}

#ifndef TEST_ALL
int main(void) { return run_test_tiermem() == 0 ? 0 : 1; }
#endif
