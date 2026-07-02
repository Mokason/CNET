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
    snprintf(p,sizeof(p),"%s/idf.bin",dir); remove(p); }

static void test_controlled(void){
    reset_store("tfidf_ctl");
    TileMemory *m=tilemem_open("tfidf_ctl",256,1000,0.99);
    const char *ai[]={"ai topic alpha","ai topic beta","ai topic gamma","ai topic delta",
                      "ai topic epsilon","ai topic zeta","ai topic eta","ai topic theta"};
    for(int i=0;i<8;i++) tilemem_ingest(m,ai[i],ai[i],"","c");
    tilemem_ingest(m,"mile zone region","mile zone region","","c");
    TileHit h[3]; int n=tilemem_search(m,"ai mile",3,h,3);
    CHECK(n>0,"search returns hits");
    CHECK(n>0 && strstr(h[0].value,"mile")!=NULL,"idf surfaces the rare-term tile first (not an ai-heavy tile)");
    tilemem_close(m);
}

static unsigned char *readfile(const char *p,size_t*len){ FILE*f=fopen(p,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0){fclose(f);return NULL;}
    unsigned char*b=malloc(sz); *len=fread(b,1,sz,f); fclose(f); return b; }

static void test_last_mile(void){
    const char *path="C:/Users/Mokason/Downloads/aivalueplaybook.pdf";
    FILE *pr=fopen(path,"rb"); if(!pr){ printf("[book] not found - skipping\n"); return; } fclose(pr);
    size_t len=0; unsigned char *buf=readfile(path,&len);
    char *text=malloc(len*4+16); size_t tl=0;
    if(pdf_extract_text(buf,len,text,len*4+16,&tl)!=PDF_OK){ free(text); free(buf); return; }
    StrList s; strlist_init(&s); corpus_split(text,&s);
    reset_store("tfidf_book");
    TileMemory *m=tilemem_open("tfidf_book",256,9000,0.6);
    clock_t t0=clock();
    for(size_t i=0;i<s.count;i++) if(corpus_quality_keep(s.lines[i])) tilemem_ingest(m,s.lines[i],s.lines[i],"","book");
    double ing=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;

    TileHit h[5]; t0=clock(); int n=tilemem_search(m,"what is the last mile problem in ai projects",5,h,5);
    double q=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;
    printf("\n[bench] ingest(df build) %.0f ms ; search(idf) %.2f ms ; tiles=%zu\n", ing, q, tilemem_total(m));
    printf("[lastmile] top-%d for 'last mile problem in ai projects':\n", n);
    int found=0;
    for(int i=0;i<n;i++){ printf("   [%.3f] %.90s\n", h[i].score, h[i].value); if(strstr(h[i].value,"last mile")) found=1; }
    CHECK(found,"a 'last mile' sentence is now in the top-5 (idf fixed the miss)");
    tilemem_close(m);
    free(text); free(buf); strlist_free(&s);
}

int run_test_tfidf(void){
    printf("=== test_tfidf ===\n");
    test_controlled();
    test_last_mile();
    printf("\n%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}

#ifndef TEST_ALL
int main(void) { return run_test_tfidf() == 0 ? 0 : 1; }
#endif
