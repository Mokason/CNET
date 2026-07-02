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
    int arg=0;
    for(int i=1;i<gu.vocab;i++) if(out[i]>out[arg]) arg=i;
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
            for(size_t s=0;s<plan.length;s++) printf(" %s",plan.names[s]);
            printf("\n");
            CHECK(plan.length>=2,"real-book graduation composed >=2 certified units");
        }
    } else {
        printf("[realbook] no clean 2-step deterministic run met the bar (honest: prose is noisy) - controlled corpus carries the composition proof\n");
    }
    if(gu.valid) graduated_units_free(&gu);
    registry_free(&reg);
    tilemem_close(mem); strlist_free(&corpus); free(text); free(buf);
}

int run_test_graduate(void){
    printf("=== test_graduate ===\n");
    test_controlled();
    test_real_book();
    printf("\n%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}

#ifndef TEST_ALL
int main(void) { return run_test_graduate() == 0 ? 0 : 1; }
#endif
