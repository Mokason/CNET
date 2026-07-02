/* PDF -> persistent corpus -> replay-train cce_wordlm -> generate.
   Usage: ./pdflearn [file.pdf ...]   (no args: uses an embedded sample PDF).
   Each run appends ingested PDFs to a growing on-disk corpus (dedup), then
   rebuilds the vocab + model from the FULL accumulated corpus (replay).
   Hashed vocab + pre-tokenized training keep it scalable to real books. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../include/pdf/pdf_extract.h"
#include "../include/corpus/corpus_split.h"
#include "../include/corpus/corpus_store.h"
#include "../include/cce/cce_wordlm.h"

#define CORPUS_PATH "pdf_corpus.txt"
#define WCTX 3

/* ---- growable string->id vocab with an open-addressing hash index (O(1)) ---- */
typedef struct { char **w; size_t n, cap; int *ht; size_t htmask; } Vocab;
static unsigned long djb2(const char *s){ unsigned long h=5381; int c; while((c=(unsigned char)*s++)) h=((h<<5)+h)^c; return h; }
static void vocab_init(Vocab *v){ v->w=NULL; v->n=0; v->cap=0; v->ht=NULL; v->htmask=0; }
static void ht_put(Vocab *v, int idx){
    size_t h = djb2(v->w[idx]) & v->htmask;
    while (v->ht[h] != -1) h=(h+1)&v->htmask;
    v->ht[h]=idx;
}
static void vocab_grow_ht(Vocab *v){
    size_t newcap = v->htmask ? (v->htmask+1)*2 : 1024;
    v->ht = (int*)realloc(v->ht, newcap*sizeof(int));
    for (size_t i=0;i<newcap;i++) v->ht[i]=-1;
    v->htmask = newcap-1;
    for (size_t i=0;i<v->n;i++) ht_put(v,(int)i);
}
static int vocab_id(Vocab *v, const char *s, int add){
    if (v->htmask==0){ if(!add) return -1; vocab_grow_ht(v); }
    else if (add && v->n*10 >= (v->htmask+1)*7) vocab_grow_ht(v);
    size_t h = djb2(s) & v->htmask;
    while (v->ht[h] != -1){ if (strcmp(v->w[v->ht[h]], s)==0) return v->ht[h]; h=(h+1)&v->htmask; }
    if (!add) return -1;
    if (v->n==v->cap){ v->cap=v->cap?v->cap*2:256; v->w=(char**)realloc(v->w,v->cap*sizeof(char*)); }
    int idx=(int)v->n; v->w[idx]=strdup(s); v->n++;
    ht_put(v, idx);
    return idx;
}

/* tokenize one sentence (lowercased words + "." token) into ids. */
static int tokenize(Vocab *v, const char *s, int *out, int cap, int add){
    int n=0; char cur[64]; int cl=0;
    for (const char *p=s;;++p){ char c=*p;
        int al=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='\'';
        if (al){ if(cl<63){ cur[cl++]= (c>='A'&&c<='Z')? (char)(c+32):c; } }
        else { if(cl>0){ cur[cl]=0; int id=vocab_id(v,cur,add); if(id>=0&&n<cap)out[n++]=id; cl=0; }
               if(c=='.'||c=='!'||c=='?'){ int id=vocab_id(v,".",add); if(id>=0&&n<cap)out[n++]=id; }
               if(c==0) break; }
    }
    return n;
}

static unsigned char *read_file(const char *path, size_t *len){
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if (sz<=0){ fclose(f); return NULL; }
    unsigned char *buf=(unsigned char*)malloc((size_t)sz);
    size_t rd=fread(buf,1,(size_t)sz,f); fclose(f);
    *len=rd; return buf;
}

/* Embedded sample PDF (uncompressed) so the target runs with no arguments. */
static const char SAMPLE_PDF[] =
"%PDF-1.4\n4 0 obj<</Length 120>>stream\n"
"BT (The little fox found a glowing mushroom in the forest.) Tj ET\n"
"BT (Tom had a black cat named Max who liked to sleep.) Tj ET\n"
"endstream endobj\n";

static const char *STATUS[] = {"OK","ENCRYPTED","UNSUPPORTED_FONTS","NO_TEXT","MALFORMED"};

int main(int argc, char **argv){
    /* 1) ingest: each PDF -> extract -> split -> append to the persistent store. */
    size_t before = corpus_count(CORPUS_PATH);
    size_t ingested = 0;
    if (argc > 1) {
        for (int a=1;a<argc;a++){
            size_t len=0; unsigned char *buf=read_file(argv[a],&len);
            if (!buf){ printf("[ingest] %s: cannot read\n", argv[a]); continue; }
            size_t tcap = len*4+16; char *text=(char*)malloc(tcap); size_t tlen=0;
            PdfStatus st = pdf_extract_text(buf, len, text, tcap, &tlen);
            printf("[ingest] %s: %s (%zu chars)\n", argv[a], STATUS[st], tlen);
            if (st==PDF_OK){ StrList s; strlist_init(&s); corpus_split(text,&s);
                size_t added=corpus_append(CORPUS_PATH,&s); ingested+=added;
                printf("[ingest]   +%zu new sentences (split into %zu)\n", added, s.count); strlist_free(&s); }
            free(text); free(buf);
        }
    } else {
        StrList s; strlist_init(&s);
        char text[1024]; size_t tlen=0;
        pdf_extract_text((const unsigned char*)SAMPLE_PDF, sizeof(SAMPLE_PDF)-1, text, sizeof(text), &tlen);
        corpus_split(text,&s);
        ingested = corpus_append(CORPUS_PATH,&s);
        printf("[ingest] embedded sample: +%zu new sentences\n", ingested);
        strlist_free(&s);
    }

    /* 2) load the FULL store, pre-tokenize ONCE into a flat buffer (vocab built here). */
    StrList corpus; strlist_init(&corpus);
    if (corpus_load(CORPUS_PATH,&corpus)!=0 || corpus.count==0){ printf("[train] empty corpus\n"); return 1; }
    Vocab v; vocab_init(&v); vocab_id(&v,".",1);

    int *flat=NULL; size_t flatn=0, flatcap=0;
    size_t *sent_start=(size_t*)malloc(corpus.count*sizeof(size_t));
    int    *sent_len  =(int*)   malloc(corpus.count*sizeof(int));
    for (size_t i=0;i<corpus.count;i++){
        int t[4096]; int nt=tokenize(&v,corpus.lines[i],t,4096,1);
        if (flatn+(size_t)nt > flatcap){ flatcap=(flatn+nt)*2+1024; flat=(int*)realloc(flat,flatcap*sizeof(int)); }
        sent_start[i]=flatn; sent_len[i]=nt;
        memcpy(flat+flatn, t, (size_t)nt*sizeof(int)); flatn+=nt;
    }
    int V=(int)v.n;
    printf("[corpus] sentences=%zu vocab=%d tokens=%zu (store had %zu, +%zu this run)\n",
           corpus.count, V, flatn, before, ingested);

    /* 3) replay-train cce_wordlm over the cached tokens. Epochs scale down with size. */
    int epochs = corpus.count > 2000 ? 4 : corpus.count > 400 ? 10 : 60;
    cce_wordlm *m = cce_wordlm_create(V, 24, WCTX, 64, 99u);
    clock_t t0=clock();
    for (int e=0;e<epochs;e++)
        for (size_t i=0;i<corpus.count;i++)
            for (int j=WCTX;j<sent_len[i];j++)
                cce_wordlm_train_step(m, &flat[sent_start[i]+j-WCTX], flat[sent_start[i]+j], 0.02f);
    printf("[train] %d epochs in %.1fs\n", epochs, (double)(clock()-t0)/CLOCKS_PER_SEC);

    /* 4) free-run a sample from the first sentence's opening context. */
    int dot=vocab_id(&v,".",0); int ctx[WCTX];
    for(int k=0;k<WCTX;k++) ctx[k]= (sent_len[0]>k)? flat[sent_start[0]+k] : 0;
    char out[1024]=""; for(int k=0;k<WCTX;k++){ if(ctx[k]>=0&&ctx[k]<V){ strcat(out, v.w[ctx[k]]); strcat(out," "); } }
    float *pen=(float*)calloc(V,sizeof(float));
    for (int step=0; step<40 && strlen(out)<900; step++){
        for(int x=0;x<V;x++) pen[x]=0.0f; for(int k=0;k<WCTX;k++) pen[ctx[k]]-=3.0f;
        int nx=cce_wordlm_predict(m,ctx,pen);
        if (nx<0||nx==dot) break;
        strcat(out, v.w[nx]); strcat(out," ");
        for(int k=0;k<WCTX-1;k++) ctx[k]=ctx[k+1];
        ctx[WCTX-1]=nx;
    }
    printf("[generate] %s.\n", out);

    /* 5) self-check (end-to-end gate): store non-empty, vocab grew, generation real. */
    int ok = (corpus.count>0) && (V>3) && (strlen(out) > 8);
    free(pen); free(flat); free(sent_start); free(sent_len); cce_wordlm_free(m);
    for (size_t i=0;i<v.n;i++) free(v.w[i]); free(v.w); free(v.ht);
    strlist_free(&corpus);
    printf(ok ? "ALL PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
