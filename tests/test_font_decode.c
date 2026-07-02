#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/pdf/font_decode.h"
#include "../include/pdf/pdf_extract.h"
#include "../include/corpus/corpus_split.h"
#include "../include/corpus/tile_memory.h"

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
    unsigned char raw[]={'s','p','e','c','i',31,'c'};
    FontDiffs fd; const char *pdf="/Differences[31/f_i]"; font_diffs_parse((const unsigned char*)pdf,strlen(pdf),&fd);
    char out[64];
    decode_codes(raw,sizeof(raw),NULL,out,sizeof(out));
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

    int kept=0,dropped=0; double goodsum=0;
    for(size_t i=0;i<s.count;i++){ if(corpus_quality_keep(s.lines[i])) kept++; else dropped++;
        goodsum += english_likeness(s.lines[i]); }
    double goodfrac = s.count? goodsum/s.count : 0;

    const char *lig[]={"specific","efficient","first","field","final","office","benefit","define","difficult","financial","fifty"};
    int recovered=0; for(int k=0;k<(int)(sizeof(lig)/sizeof(lig[0]));k++) if(strstr(text,lig[k])) recovered++;

    printf("\n[bench] extract+decode %.0f ms, %zu chars\n", ms, tl);
    printf("[book] sentences=%zu  mean english-likeness=%.2f  quality-kept=%d dropped=%d\n",
           s.count, goodfrac, kept, dropped);
    printf("[book] ligature words recovered (of 11 probes): %d\n", recovered);
    CHECK(goodfrac>0.6,"corpus is mostly English-like after decode");
    CHECK(recovered>0,"at least one ligature word recovered (specific/first/...)");
    CHECK(dropped>0,"quality filter dropped some junk");

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

int run_test_fontdecode(void){
    printf("=== test_font_decode ===\n");
    test_glyph();
    test_diffs();
    test_decode_pick();
    test_quality();
    test_real_book();
    printf("\n%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}

#ifndef TEST_ALL
int main(void) { return run_test_fontdecode() == 0 ? 0 : 1; }
#endif
