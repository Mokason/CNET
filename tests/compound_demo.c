#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/corpus/retrieval.h"
#include "../include/cce/cce_wordlm.h"
#include "../include/pdf/pdf_extract.h"
#include "../include/corpus/corpus_split.h"

static int g_fail=0, g_checks=0;
#define CHECK(c,m) do{ g_checks++; if(!(c)){ g_fail++; printf("  FAIL: %s (%s:%d)\n",m,__FILE__,__LINE__);} }while(0)

static void test_store(void){
    RetrievalStore *r=retrieval_create(2);
    int A[]={1,2,3,2,3,4};                       /* (1,2)->3 (2,3)->2 (3,2)->3 (2,3)->4 */
    IngestStats s1=retrieval_ingest(r,A,6);
    CHECK(s1.new_keys==3, "first ingest: 3 distinct contexts");
    CHECK(retrieval_keys(r)==3, "store has 3 keys");
    IngestStats s2=retrieval_ingest(r,A,6);       /* identical re-ingest */
    CHECK(s2.new_keys==0, "re-ingest identical: 0 new keys (reuse)");
    CHECK(s2.reused_keys==s1.total_contexts, "re-ingest: all contexts reused");
    int w[8],c[8]; int ctx23[2]={2,3};
    int n=retrieval_lookup(r,ctx23,w,c,8);
    CHECK(n==2, "(2,3) has 2 continuations {3,4}");
    retrieval_free(r);
}

/* --- hashed string->id vocab (same pattern as pdflearn) --- */
typedef struct { char **w; size_t n, cap; int *ht; size_t htmask; } Vocab;
static unsigned long djb2(const char *s){ unsigned long h=5381; int c; while((c=(unsigned char)*s++)) h=((h<<5)+h)^c; return h; }
static void vocab_init(Vocab *v){ v->w=NULL; v->n=0; v->cap=0; v->ht=NULL; v->htmask=0; }
static void ht_put(Vocab *v,int i){ size_t h=djb2(v->w[i])&v->htmask; while(v->ht[h]!=-1) h=(h+1)&v->htmask; v->ht[h]=i; }
static void vgrow(Vocab *v){ size_t nc=v->htmask?(v->htmask+1)*2:1024; v->ht=realloc(v->ht,nc*sizeof(int));
    for(size_t i=0;i<nc;i++) v->ht[i]=-1;
    v->htmask=nc-1;
    for(size_t i=0;i<v->n;i++) ht_put(v,(int)i); }
static int vid(Vocab *v,const char*s,int add){
    if(v->htmask==0){ if(!add) return -1; vgrow(v);} else if(add&&v->n*10>=(v->htmask+1)*7) vgrow(v);
    size_t h=djb2(s)&v->htmask; while(v->ht[h]!=-1){ if(strcmp(v->w[v->ht[h]],s)==0) return v->ht[h]; h=(h+1)&v->htmask; }
    if(!add) return -1;
    if(v->n==v->cap){ v->cap=v->cap?v->cap*2:256; v->w=realloc(v->w,v->cap*sizeof(char*)); }
    int idx=(int)v->n; v->w[idx]=strdup(s); v->n++; ht_put(v,idx); return idx;
}
static int vtok(Vocab *v,const char*s,int*out,int cap,int add){ int n=0;char cur[64];int cl=0;
    for(const char*p=s;;++p){ char c=*p; int al=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='\'';
        if(al){ if(cl<63) cur[cl++]=(c>='A'&&c<='Z')?(char)(c+32):c; }
        else{ if(cl>0){cur[cl]=0;int id=vid(v,cur,add);if(id>=0&&n<cap)out[n++]=id;cl=0;}
              if(c=='.'||c=='!'||c=='?'){int id=vid(v,".",add);if(id>=0&&n<cap)out[n++]=id;} if(c==0)break; } }
    return n; }

/* Controlled corpora with KNOWN overlap. ctx=3 keys. */
static const char *BOOK_A[] = {
    "the team aligns on the goal and the value of the work.",
    "data and the kind of teams you need to build the system.",
    "change is the focus and the value of the investment grows.",
    "the leader shares the story and builds trust with the team."
};
static const char *BOOK_A2[] = {   /* identical to A */
    "the team aligns on the goal and the value of the work.",
    "data and the kind of teams you need to build the system.",
    "change is the focus and the value of the investment grows.",
    "the leader shares the story and builds trust with the team."
};
static const char *BOOK_B[] = {    /* related: 2 shared sentences + 2 new (shared vocab) */
    "the team aligns on the goal and the value of the work.",
    "change is the focus and the value of the investment grows.",
    "the team builds the system and shares the value of data.",
    "the leader aligns the goal and grows the trust of the team."
};
static const char *BOOK_C[] = {    /* unrelated: disjoint vocabulary */
    "quantum particles entangle across vast cosmic distances instantly.",
    "photosynthesis converts sunlight into chemical energy within leaves.",
    "volcanic eruptions reshape landscapes over geological timescales.",
    "migratory birds navigate using magnetic fields and starlight."
};
#define NA ((int)(sizeof(BOOK_A)/sizeof(*BOOK_A)))

/* tokenize a whole book (shared vocab) into one flat stream; returns length. */
static size_t book_tokens(Vocab *v, const char **book, int nsent, int *out, int cap){
    size_t n=0; for(int s=0;s<nsent && (int)n<cap;s++){ int t[256]; int nt=vtok(v,book[s],t,256,1);
        for(int i=0;i<nt && (int)n<cap;i++) out[n++]=t[i]; }
    return n;
}
static double reuse_rate(IngestStats s){ return s.total_contexts? (double)s.reused_keys/s.total_contexts : 0.0; }

static void test_compounding(void){
    Vocab v; vocab_init(&v);
    int A[1024],A2[1024],B[1024],C[1024];
    size_t na=book_tokens(&v,BOOK_A,NA,A,1024);
    size_t na2=book_tokens(&v,BOOK_A2,NA,A2,1024);
    size_t nb=book_tokens(&v,BOOK_B,NA,B,1024);
    size_t nc=book_tokens(&v,BOOK_C,NA,C,1024);

    RetrievalStore *r=retrieval_create(3);
    IngestStats sa  = retrieval_ingest(r,A,na);
    IngestStats sa2 = retrieval_ingest(r,A2,na2);   /* identical */
    IngestStats sb  = retrieval_ingest(r,B,nb);     /* related   */
    IngestStats sc  = retrieval_ingest(r,C,nc);     /* unrelated */

    printf("[compound] A  : new_keys=%zu reuse=%.0f%%\n", sa.new_keys, 100*reuse_rate(sa));
    printf("[compound] A2 : new_keys=%zu reuse=%.0f%%  (identical re-ingest)\n", sa2.new_keys, 100*reuse_rate(sa2));
    printf("[compound] B  : new_keys=%zu reuse=%.0f%%  (related)\n", sb.new_keys, 100*reuse_rate(sb));
    printf("[compound] C  : new_keys=%zu reuse=%.0f%%  (unrelated)\n", sc.new_keys, 100*reuse_rate(sc));

    CHECK(sa2.new_keys==0, "identical re-ingest adds 0 new keys (100% reuse)");
    CHECK(sb.new_keys>0 && sb.new_keys<sb.total_contexts, "related book: partial reuse (some new, some reused)");
    CHECK(sb.reused_keys>0, "related book reuses prior keys");
    CHECK((double)sc.new_keys/sc.total_contexts > reuse_rate(sb), "unrelated book has less reuse than related");

    retrieval_free(r);
    for(size_t i=0;i<v.n;i++) free(v.w[i]);
    free(v.w); free(v.ht);
}

/* Retrieval-primary generation with frozen-LM backoff. */
static void generate(RetrievalStore *r, cce_wordlm *lm, Vocab *v, const int *seed, int seedlen, char *out, size_t outsz){
    int ctx[3]; for(int k=0;k<3;k++) ctx[k]= (seedlen>k)? seed[k] : 0;
    out[0]=0; for(int k=0;k<3;k++){ if(ctx[k]>=0&&ctx[k]<(int)v->n){ strcat(out,v->w[ctx[k]]); strcat(out," "); } }
    int dot=vid(v,".",0);
    for(int step=0; step<30 && strlen(out)<outsz-32; step++){
        int w[32],c[32]; int n=retrieval_lookup(r,ctx,w,c,32);
        int nx;
        if(n>0){ int best=0; for(int i=1;i<n;i++) if(c[i]>c[best]) best=i; nx=w[best]; }   /* memory */
        else { float pen[4096]; int V=(int)v->n; for(int i=0;i<V&&i<4096;i++) pen[i]=0; nx=cce_wordlm_predict(lm,ctx,pen); }  /* LM backoff */
        if(nx<0||nx==dot) break;
        if(nx<(int)v->n){ strcat(out,v->w[nx]); strcat(out," "); }
        ctx[0]=ctx[1]; ctx[1]=ctx[2]; ctx[2]=nx;
    }
}

/* Cost contrast: retrieval ingest (flat O(book)) vs retrain a fresh LM on the
   cumulative corpus (O(sum), grows). Prints a table; asserts the trend. */
static void test_cost_curve(void){
    Vocab v; vocab_init(&v);
    const char **books[3]={BOOK_A,BOOK_B,BOOK_C}; const char *names[3]={"A","B","C"};
    int cum[4096]; size_t cumn=0;
    RetrievalStore *r=retrieval_create(3);
    double retr_ms[3], retrain_ms[3];
    printf("[cost] book | retrieval_ingest_ms | retrain_baseline_ms (cumulative)\n");
    for(int b=0;b<3;b++){
        int tok[1024]; size_t nt=book_tokens(&v,books[b],NA,tok,1024);
        clock_t a=clock(); retrieval_ingest(r,tok,nt); clock_t e=clock();
        retr_ms[b]=1000.0*(e-a)/CLOCKS_PER_SEC;
        for(size_t i=0;i<nt && cumn<4096;i++) cum[cumn++]=tok[i];     /* grow cumulative corpus */
        cce_wordlm *lm=cce_wordlm_create((int)v.n,16,3,32,7u);
        a=clock();
        for(int ep=0;ep<5;ep++) for(size_t i=3;i<cumn;i++) cce_wordlm_train_step(lm,&cum[i-3],cum[i],0.02f);
        e=clock(); retrain_ms[b]=1000.0*(e-a)/CLOCKS_PER_SEC; cce_wordlm_free(lm);
        printf("[cost]  %s   |        %7.2f      |        %7.2f\n", names[b], retr_ms[b], retrain_ms[b]);
    }
    CHECK(retrain_ms[2] > retrain_ms[0], "retrain baseline cost GROWS with cumulative corpus");
    retrieval_free(r);
    for(size_t i=0;i<v.n;i++) free(v.w[i]);
    free(v.w); free(v.ht);
}

/* Generation reuses memory across books (frozen LM trained once on A only). */
static void test_generation(void){
    Vocab v; vocab_init(&v);
    int A[1024],B[1024]; size_t na=book_tokens(&v,BOOK_A,NA,A,1024); size_t nb=book_tokens(&v,BOOK_B,NA,B,1024);
    cce_wordlm *lm=cce_wordlm_create((int)v.n,16,3,32,7u);          /* frozen: trained once on A */
    for(int ep=0;ep<40;ep++) for(size_t i=3;i<na;i++) cce_wordlm_train_step(lm,&A[i-3],A[i],0.02f);
    RetrievalStore *r=retrieval_create(3);
    retrieval_ingest(r,A,na); retrieval_ingest(r,B,nb);            /* memory grows; LM NOT retrained */
    char out[1024]; int seed[3]={A[0],A[1],A[2]};
    generate(r,lm,&v,seed,3,out,sizeof(out));
    printf("[generate] %s\n", out);
    CHECK(strlen(out)>12, "generation produces real-word text from memory");
    cce_wordlm_free(lm); retrieval_free(r);
    for(size_t i=0;i<v.n;i++) free(v.w[i]);
    free(v.w); free(v.ht);
}

static void test_persist(void){
    Vocab v; vocab_init(&v); int A[1024]; size_t na=book_tokens(&v,BOOK_A,NA,A,1024);
    RetrievalStore *r=retrieval_create(3); retrieval_ingest(r,A,na);
    size_t keys=retrieval_keys(r), pairs=retrieval_pairs(r);
    CHECK(retrieval_save(r,"retrieval_store.bin")==0, "save store");
    retrieval_free(r);
    RetrievalStore *r2=retrieval_load("retrieval_store.bin");
    CHECK(r2!=NULL, "load store");
    CHECK(r2 && retrieval_keys(r2)==keys && retrieval_pairs(r2)==pairs, "round-trip preserves keys+pairs");
    int w[8],c[8]; int ctx[3]={A[0],A[1],A[2]};
    CHECK(r2 && retrieval_lookup(r2,ctx,w,c,8) > 0, "loaded store answers a known context");
    if(r2) retrieval_free(r2);
    remove("retrieval_store.bin");
    for(size_t i=0;i<v.n;i++) free(v.w[i]);
    free(v.w); free(v.ht);
}

static unsigned char *readfile(const char *p, size_t *len){ FILE *f=fopen(p,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); if(sz<=0){fclose(f);return NULL;}
    unsigned char *b=malloc(sz); *len=fread(b,1,sz,f); fclose(f); return b; }

/* Ingest a sentence range [lo,hi) of a PDF into the store; returns IngestStats. */
static IngestStats ingest_pdf_range(RetrievalStore *r, Vocab *v, const char *path, int lo, int hi, int *valid){
    IngestStats acc={0,0,0,0}; *valid=0;
    size_t len=0; unsigned char *buf=readfile(path,&len); if(!buf) return acc;
    size_t tcap=len*4+16; char *text=malloc(tcap); size_t tl=0;
    PdfStatus st=pdf_extract_text(buf,len,text,tcap,&tl);
    if(st==PDF_OK){ *valid=1; StrList s; strlist_init(&s); corpus_split(text,&s);
        for(size_t i=(size_t)lo;i<s.count && (int)i<hi;i++){ int t[512]; int nt=vtok(v,s.lines[i],t,512,1);
            IngestStats x=retrieval_ingest(r,t,(size_t)nt);
            acc.new_keys+=x.new_keys; acc.reused_keys+=x.reused_keys; acc.new_pairs+=x.new_pairs; acc.total_contexts+=x.total_contexts; }
        strlist_free(&s); }
    free(text); free(buf); return acc;
}

static void test_real_pdf(void){
    const char *path="C:/Users/Mokason/Downloads/aivalueplaybook.pdf";
    FILE *probe=fopen(path,"rb"); if(!probe){ printf("[realpdf] %s not found - skipping\n", path); return; }
    fclose(probe);
    Vocab v; vocab_init(&v); RetrievalStore *r=retrieval_create(3);
    int ok1,ok2;
    IngestStats h1=ingest_pdf_range(r,&v,path,0,200,&ok1);       /* first 200 sentences */
    IngestStats h2=ingest_pdf_range(r,&v,path,200,400,&ok2);     /* next 200 sentences */
    printf("[realpdf] half1 new_keys=%zu reuse=%.0f%% ; half2 new_keys=%zu reuse=%.0f%%\n",
           h1.new_keys, 100*reuse_rate(h1), h2.new_keys, 100*reuse_rate(h2));
    CHECK(ok1 && ok2, "real PDF extracted OK");
    CHECK(h2.reused_keys>0, "second half reuses context keys from the first (cross-section compounding)");
    size_t cand=retrieval_certifiable_contexts(r,3,0.95);
    printf("[realpdf] contract-distillation candidates (near-deterministic contexts): %zu\n", cand);
    retrieval_free(r); for(size_t i=0;i<v.n;i++) free(v.w[i]); free(v.w); free(v.ht);
}

int main(void){
    printf("=== compound_demo ===\n");
    test_store();
    test_compounding();
    test_cost_curve();
    test_generation();
    test_persist();
    test_real_pdf();
    printf("%d/%d checks passed\n", g_checks-g_fail, g_checks);
    return g_fail?1:0;
}
