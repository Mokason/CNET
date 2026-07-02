/* Tiered, fuzzy, decaying tile memory (AICIMO-adapted: TiledMemoryIndex hashed-vector
   recall + GraphMemoryHead decay), in C. Passages -> tiles; fuzzy-dedup ingest; a
   capacity-bounded HOT tier spills to a WARM on-disk tier; the store persists. */
#include "../../include/corpus/tile_memory.h"
#include "../../include/corpus/synonyms.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p,0755)
#endif

/* Real term -> document-frequency dictionary (no hash-bucket collisions, so idf can
   isolate a rare term like "mile"). Open-addressing string hash. */
typedef struct { char **key; unsigned *df; unsigned **post; int *postn, *postcap; size_t cap, mask, n; } TermDF;

typedef struct { size_t where; int tier; char live; } Loc;  /* tier 0=HOT(where=hot idx) 1=WARM(where=offset) */

struct TileMemory {
    int dim, hot_cap; double tau;
    char store_dir[256];
    Tile *hot; int n_hot, cap_hot;
    size_t warm_count;
    TermDF tdf; size_t doc_count;     /* term->df dictionary + tile count (TF-IDF) */
    char **res; int n_res, cap_res;   /* search-result value arena (freed each search) */
    Synonyms *syn; int syn_dirty;     /* Lever 3: PPMI synonym map + stale flag */
    double syn_alpha; int syn_k, syn_min_cooc, syn_min_df, syn_max_expand;
    double syn_df_frac, syn_pmi_scale; int syn_discount;
    Loc *loc; size_t loc_cap; unsigned next_tid;   /* tid -> location (presence/live = liveness) */
    unsigned *seen; size_t seen_cap, seen_epoch;   /* per-search candidate marker */
};

/* ---- AICIMO-ported tokenization + FNV-1a hashed dense vector ---- */
static unsigned fnv1a(const char *s){ unsigned h=2166136261u; for(;*s;s++){ h^=(unsigned char)*s; h*=16777619u; } return h; }
static int is_stop(const char *t){
    static const char *S[]={"the","and","for","from","with","that","this","into","should","would","could",
        "what","when","where","which","note","are","was","its","has","have","not","you","your","our",0};
    for(int i=0;S[i];i++) if(strcmp(S[i],t)==0) return 1;
    return 0;
}
static void stem(char *t){ size_t n=strlen(t);
    if(n>4 && strcmp(t+n-3,"ies")==0){ t[n-3]='y'; t[n-2]=0; }
    else if(n>3 && t[n-1]=='s'){ t[n-1]=0; }
}
static void vectorize(const char *text, float *vec, int dim){
    memset(vec,0,(size_t)dim*sizeof(float));
    char tok[64]; int tl=0;
    for(const char *p=text;;p++){ char c=*p;
        int al=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
        if(al){ if(tl<63) tok[tl++]=(c>='A'&&c<='Z')?(char)(c+32):c; }
        else { if(tl>0){ tok[tl]=0; stem(tok); if(strlen(tok)>1 && !is_stop(tok)) vec[fnv1a(tok)%(unsigned)dim]+=1.0f; tl=0; }
               if(c==0) break; } }
    double s=0; for(int i=0;i<dim;i++) s+=(double)vec[i]*vec[i];
    if(s>0){ float inv=1.0f/(float)sqrt(s); for(int i=0;i<dim;i++) vec[i]*=inv; }
}
static float cosine(const float *a, const float *b, int dim){ float d=0; for(int i=0;i<dim;i++) d+=a[i]*b[i]; return d; }
/* (search is term-dictionary TF-IDF below; the dense vec/cosine is for fuzzy-dedup only) */

/* ---- term -> df dictionary + tokenizer (TF-IDF search) ---- */
static unsigned long tdf_hash(const char *s){ unsigned long h=5381; int c; while((c=(unsigned char)*s++)) h=((h<<5)+h)^c; return h; }
static void tdf_init(TermDF *t){ memset(t,0,sizeof(*t)); }
static void tdf_free(TermDF *t){ for(size_t i=0;i<t->cap;i++){ free(t->key[i]); free(t->post[i]); }
    free(t->key); free(t->df); free(t->post); free(t->postn); free(t->postcap); tdf_init(t); }
static void tdf_grow(TermDF *t){
    size_t nc=t->cap?t->cap*2:1024, nm=nc-1;
    char **nk=(char**)calloc(nc,sizeof(char*)); unsigned *nd=(unsigned*)calloc(nc,sizeof(unsigned));
    unsigned **np=(unsigned**)calloc(nc,sizeof(unsigned*));
    int *npn=(int*)calloc(nc,sizeof(int)); int *npc=(int*)calloc(nc,sizeof(int));
    for(size_t i=0;i<t->cap;i++) if(t->key[i]){ size_t h=tdf_hash(t->key[i])&nm; while(nk[h]) h=(h+1)&nm;
        nk[h]=t->key[i]; nd[h]=t->df[i]; np[h]=t->post[i]; npn[h]=t->postn[i]; npc[h]=t->postcap[i]; }
    free(t->key); free(t->df); free(t->post); free(t->postn); free(t->postcap);
    t->key=nk; t->df=nd; t->post=np; t->postn=npn; t->postcap=npc; t->cap=nc; t->mask=nm;
}
static int tdf_bucket(TermDF *t, const char *s, int add){
    if(t->cap==0){ if(!add) return -1; tdf_grow(t); } else if(add && t->n*10>=t->cap*7) tdf_grow(t);
    size_t h=tdf_hash(s)&t->mask;
    while(t->key[h]){ if(strcmp(t->key[h],s)==0) return (int)h; h=(h+1)&t->mask; }
    if(!add) return -1;
    t->key[h]=strdup(s); t->df[h]=0; t->post[h]=NULL; t->postn[h]=0; t->postcap[h]=0; t->n++; return (int)h;
}
static unsigned *tdf_slot(TermDF *t, const char *s, int add){ int h=tdf_bucket(t,s,add); return h<0?NULL:&t->df[h]; }
static void tdf_post(TermDF *t, const char *s, unsigned tid){
    int h=tdf_bucket(t,s,1); if(h<0) return;
    if(t->postn[h]==t->postcap[h]){ t->postcap[h]=t->postcap[h]?t->postcap[h]*2:4;
        t->post[h]=(unsigned*)realloc(t->post[h],(size_t)t->postcap[h]*sizeof(unsigned)); }
    t->post[h][t->postn[h]++]=tid;
}
static unsigned *tdf_postings(TermDF *t, const char *s, int *n){ int h=tdf_bucket(t,s,0);
    if(h<0){ *n=0; return NULL; } *n=t->postn[h]; return t->post[h]; }
/* tokenize text into lowercased, stemmed, non-stop terms (len>1). Returns count. */
static int tokenize_terms(const char *s, char out[][32], int cap){
    int n=0; char tok[32]; int tl=0;
    for(const char *p=s;;p++){ char c=*p;
        int al=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
        if(al){ if(tl<31) tok[tl++]=(c>='A'&&c<='Z')?(char)(c+32):c; }
        else { if(tl>0){ tok[tl]=0; stem(tok); if(strlen(tok)>1 && !is_stop(tok) && n<cap){ snprintf(out[n],32,"%s",tok); n++; } tl=0; }
               if(c==0) break; } }
    return n;
}
/* distinct (deduped) terms of a tile key into `out` (pointers into `store`). Returns count. */
static int tile_distinct_terms(const char *key, char store[][32], const char **out, int cap){
    int nt=tokenize_terms(key,store,cap), nd=0;
    for(int a=0;a<nt;a++){ int dup=0; for(int b=0;b<nd;b++) if(strcmp(store[a],out[b])==0){ dup=1; break; }
        if(!dup) out[nd++]=store[a]; }
    return nd;
}
/* document-frequency callback for syn_finalize (reads the term->df dictionary). */
static unsigned tm_df_of(void *ctx, const char *term){
    TileMemory *m=(TileMemory*)ctx; unsigned *d=tdf_slot(&m->tdf,(char*)term,0); return d? *d:0;
}

static void slugify(const char *text, char *out, size_t cap){
    size_t o=0; int prev_us=0;
    for(const char *p=text; *p && o+1<cap; p++){ char c=*p;
        if((c>='a'&&c<='z')||(c>='0'&&c<='9')){ out[o++]=c; prev_us=0; }
        else if(c>='A'&&c<='Z'){ out[o++]=(char)(c+32); prev_us=0; }
        else if(!prev_us && o>0){ out[o++]='_'; prev_us=1; } }
    while(o>0 && out[o-1]=='_') o--;
    out[o]=0; if(o==0) snprintf(out,cap,"tile");
}

static void tile_free(Tile *t){ free(t->key); free(t->value); free(t->vec); }

/* ---- persistence ---- */
static void warm_path(const TileMemory *m, char *out, size_t cap){ snprintf(out,cap,"%s/warm.bin",m->store_dir); }
static void hot_path (const TileMemory *m, char *out, size_t cap){ snprintf(out,cap,"%s/hot.bin", m->store_dir); }
static void idf_path (const TileMemory *m, char *out, size_t cap){ snprintf(out,cap,"%s/idf.bin", m->store_dir); }
static void syn_path (const TileMemory *m, char *out, size_t cap){ snprintf(out,cap,"%s/synonyms.bin",m->store_dir); }
static void wstr(FILE *f, const char *s){ int n=(int)strlen(s); fwrite(&n,sizeof(int),1,f); fwrite(s,1,(size_t)n,f); }
static char *rstr(FILE *f){ int n; if(fread(&n,sizeof(int),1,f)!=1||n<0||n>1000000) return NULL;
    char *s=(char*)malloc((size_t)n+1); if(!s) return NULL; if(fread(s,1,(size_t)n,f)!=(size_t)n){ free(s); return NULL; } s[n]=0; return s; }
static void tile_write(FILE *f, const Tile *t){
    fwrite(t->id,1,64,f); fwrite(t->label,1,32,f); fwrite(t->source,1,64,f);
    fwrite(&t->heat,sizeof(int),1,f); fwrite(&t->count,sizeof(int),1,f);
    wstr(f,t->key); wstr(f,t->value);
}
static int tile_read(FILE *f, Tile *t, int dim){
    memset(t,0,sizeof(*t));
    if(fread(t->id,1,64,f)!=64) return -1;
    if(fread(t->label,1,32,f)!=32) return -1;
    if(fread(t->source,1,64,f)!=64) return -1;
    if(fread(&t->heat,sizeof(int),1,f)!=1) return -1;
    if(fread(&t->count,sizeof(int),1,f)!=1) return -1;
    t->key=rstr(f); t->value=rstr(f); if(!t->key||!t->value){ free(t->key); free(t->value); return -1; }
    t->vec=(float*)malloc((size_t)dim*sizeof(float)); vectorize(t->key,t->vec,dim);
    return 0;
}
/* ---- inverted-index location array + per-search seen epoch (tid-indexed) ---- */
static void loc_ensure(TileMemory *m, unsigned tid){
    if(tid<m->loc_cap) return;
    size_t nc=m->loc_cap?m->loc_cap:256; while(tid>=nc) nc*=2;
    m->loc=(Loc*)realloc(m->loc,nc*sizeof(Loc));
    for(size_t i=m->loc_cap;i<nc;i++){ m->loc[i].where=0; m->loc[i].tier=0; m->loc[i].live=0; }
    m->loc_cap=nc;
}
static void loc_set(TileMemory *m, unsigned tid, int tier, size_t where){
    loc_ensure(m,tid); m->loc[tid].tier=tier; m->loc[tid].where=where; m->loc[tid].live=1; }
static void loc_kill(TileMemory *m, unsigned tid){ if(tid<m->loc_cap) m->loc[tid].live=0; }
static int  loc_live(TileMemory *m, unsigned tid){ return tid<m->loc_cap && m->loc[tid].live; }
static void seen_ensure(TileMemory *m){
    if(m->next_tid<=m->seen_cap) return;
    size_t nc=m->seen_cap?m->seen_cap:256; while(m->next_tid>nc) nc*=2;
    m->seen=(unsigned*)realloc(m->seen,nc*sizeof(unsigned));
    for(size_t i=m->seen_cap;i<nc;i++) m->seen[i]=0;
    m->seen_cap=nc;
}
static int cmp_size(const void *a, const void *b){ size_t x=*(const size_t*)a, y=*(const size_t*)b; return (x>y)-(x<y); }

/* Remove the live HOT tile with this tid: free, swap the last HOT tile into its slot (fixing
   that moved tile's location), and mark the tid dead. No-op if tid isn't a live HOT tile. */
static void remove_hot_by_tid(TileMemory *m, unsigned tid){
    if(!loc_live(m,tid) || m->loc[tid].tier!=0) return;
    int idx=(int)m->loc[tid].where;
    tile_free(&m->hot[idx]);
    int last=--m->n_hot; m->hot[idx]=m->hot[last];
    if(idx!=last) loc_set(m, m->hot[idx].tid, 0, (size_t)idx);
    loc_kill(m,tid);
}

static size_t warm_append(TileMemory *m, const Tile *t){
    MKDIR(m->store_dir);
    char wp[300]; warm_path(m,wp,sizeof(wp));
    FILE *f=fopen(wp,"ab"); if(!f) return (size_t)-1;
    fseek(f,0,SEEK_END); long off=ftell(f);
    tile_write(f,t); fclose(f); m->warm_count++;
    return (size_t)off;
}
static void spill_coldest(TileMemory *m){
    if(m->n_hot==0) return;
    int b=0; for(int i=1;i<m->n_hot;i++)
        if(m->hot[i].heat<m->hot[b].heat || (m->hot[i].heat==m->hot[b].heat && m->hot[i].count<m->hot[b].count)) b=i;
    size_t off=warm_append(m,&m->hot[b]);
    loc_set(m, m->hot[b].tid, 1, off);          /* spilled tile now lives in WARM */
    tile_free(&m->hot[b]);
    int last=--m->n_hot; m->hot[b]=m->hot[last];
    if(b!=last) loc_set(m, m->hot[b].tid, 0, (size_t)b);   /* moved tile's new HOT index */
}
static void tilemem_load(TileMemory *m){
    char hp[300]; hot_path(m,hp,sizeof(hp)); FILE *f=fopen(hp,"rb");
    if(f){ Tile t; while(tile_read(f,&t,m->dim)==0){
        if(m->n_hot==m->cap_hot){ m->cap_hot=m->cap_hot?m->cap_hot*2:64; m->hot=(Tile*)realloc(m->hot,(size_t)m->cap_hot*sizeof(Tile)); }
        unsigned tid=m->next_tid++; t.tid=tid; int idx=m->n_hot; m->hot[m->n_hot++]=t;
        loc_set(m,tid,0,(size_t)idx);
        char tt[256][32]; const char *dt[256]; int nd=tile_distinct_terms(t.key,tt,dt,256);
        for(int a=0;a<nd;a++) tdf_post(&m->tdf,dt[a],tid);
    } fclose(f); }
    char wp[300]; warm_path(m,wp,sizeof(wp)); FILE *g=fopen(wp,"rb");
    if(g){ Tile t; for(;;){ long off=ftell(g); if(tile_read(g,&t,m->dim)!=0) break;
        m->warm_count++; unsigned tid=m->next_tid++; loc_set(m,tid,1,(size_t)off);
        char tt[256][32]; const char *dt[256]; int nd=tile_distinct_terms(t.key,tt,dt,256);
        for(int a=0;a<nd;a++) tdf_post(&m->tdf,dt[a],tid);
        tile_free(&t); } fclose(g); }
    char ip[300]; idf_path(m,ip,sizeof(ip)); FILE *h=fopen(ip,"r");
    if(h){ char line[64]; unsigned df;
        if(fscanf(h,"%zu\n",&m->doc_count)!=1) m->doc_count=0;
        while(fscanf(h,"%63s %u\n",line,&df)==2){ unsigned *d=tdf_slot(&m->tdf,line,1); if(d)*d=df; }
        fclose(h); }
    char sp[300]; syn_path(m,sp,sizeof(sp)); size_t stamp=0;
    if(syn_load(m->syn,sp,&stamp)==0){ if(stamp!=m->doc_count) m->syn_dirty=1; }
    else if(m->doc_count>0) m->syn_dirty=1;   /* tiles exist but no/stale map */
}
static void tilemem_save(TileMemory *m){
    MKDIR(m->store_dir);
    char hp[300]; hot_path(m,hp,sizeof(hp)); FILE *f=fopen(hp,"wb");
    if(f){ for(int i=0;i<m->n_hot;i++) tile_write(f,&m->hot[i]); fclose(f); }
    char ip[300]; idf_path(m,ip,sizeof(ip)); FILE *h=fopen(ip,"w");
    if(h){ fprintf(h,"%zu\n",m->doc_count);
        for(size_t i=0;i<m->tdf.cap;i++) if(m->tdf.key[i]) fprintf(h,"%s %u\n",m->tdf.key[i],m->tdf.df[i]);
        fclose(h); }
}

TileMemory *tilemem_open(const char *store_dir, int dim, int hot_cap, double dedup_tau){
    if(dim<=0||hot_cap<=0) return NULL;
    TileMemory *m=(TileMemory*)calloc(1,sizeof(*m));
    m->dim=dim; m->hot_cap=hot_cap; m->tau=dedup_tau;
    snprintf(m->store_dir,sizeof(m->store_dir),"%s",store_dir);
    tdf_init(&m->tdf);
    m->syn=syn_new();
    m->syn_alpha=0.0; m->syn_k=5; m->syn_min_cooc=2; m->syn_min_df=2;
    m->syn_df_frac=0.5; m->syn_max_expand=64; m->syn_pmi_scale=5.0; m->syn_discount=1; m->syn_dirty=0;
    tilemem_load(m);
    return m;
}
void tilemem_close(TileMemory *m){
    if(!m) return;
    tilemem_save(m);
    for(int i=0;i<m->n_hot;i++) tile_free(&m->hot[i]);
    free(m->hot);
    for(int i=0;i<m->n_res;i++) free(m->res[i]);
    free(m->res);
    tdf_free(&m->tdf);
    syn_free(m->syn);
    free(m->loc); free(m->seen);
    free(m);
}

/* ---- ingest (fuzzy-dedup) ---- */
static int hot_best(TileMemory *m, const float *qv, float *best_cos){
    int best=-1; float bc=-1;
    for(int i=0;i<m->n_hot;i++){ float c=cosine(qv,m->hot[i].vec,m->dim); if(c>bc){ bc=c; best=i; } }
    *best_cos=bc; return best;
}
int tilemem_ingest(TileMemory *m, const char *key, const char *value, const char *label, const char *source){
    float *qv=(float*)malloc((size_t)m->dim*sizeof(float)); vectorize(key,qv,m->dim);
    float bc; int b=hot_best(m,qv,&bc);
    if(b>=0 && bc>=(float)m->tau){ m->hot[b].count++; m->hot[b].heat++; free(qv); return 0; }
    while(m->n_hot>=m->hot_cap) spill_coldest(m);
    if(m->n_hot==m->cap_hot){ m->cap_hot=m->cap_hot?m->cap_hot*2:64; m->hot=(Tile*)realloc(m->hot,(size_t)m->cap_hot*sizeof(Tile)); }
    Tile *t=&m->hot[m->n_hot++]; memset(t,0,sizeof(*t));
    t->key=strdup(key); t->value=strdup(value?value:key); t->vec=qv;
    snprintf(t->label,sizeof(t->label),"%s",label?label:"");
    snprintf(t->source,sizeof(t->source),"%s",source?source:"");
    slugify(label&&*label?label:key, t->id, sizeof(t->id));
    t->heat=1; t->count=1;
    unsigned tid=m->next_tid++; t->tid=tid;
    loc_set(m,tid,0,(size_t)(m->n_hot-1));
    /* TF-IDF df bump + inverted-index posting, once per distinct term. */
    { char terms[256][32]; int nt=tokenize_terms(key,terms,256);
      for(int a=0;a<nt;a++){ int dup=0; for(int b=0;b<a;b++) if(strcmp(terms[a],terms[b])==0){ dup=1; break; }
        if(!dup){ unsigned *d=tdf_slot(&m->tdf,terms[a],1); if(d)(*d)++; tdf_post(&m->tdf,terms[a],tid); } } }
    m->doc_count++;
    m->syn_dirty=1;
    return 1;
}

void tilemem_build_synonyms(TileMemory *m){
    if(m->syn) syn_free(m->syn);
    m->syn=syn_new();
    char tt[256][32]; const char *dt[256];
    for(int i=0;i<m->n_hot;i++){ int nd=tile_distinct_terms(m->hot[i].key,tt,dt,256);
        syn_observe_tile(m->syn,dt,nd); }
    char wp[300]; warm_path(m,wp,sizeof(wp)); FILE *f=fopen(wp,"rb");
    if(f){ Tile t; while(tile_read(f,&t,m->dim)==0){
        int nd=tile_distinct_terms(t.key,tt,dt,256); syn_observe_tile(m->syn,dt,nd);
        tile_free(&t); } fclose(f); }
    syn_finalize(m->syn, tm_df_of, m, m->doc_count,
                 m->syn_k, m->syn_min_cooc, m->syn_df_frac, m->syn_min_df, m->syn_discount);
    char sp[300]; syn_path(m,sp,sizeof(sp)); syn_save(m->syn,sp,m->doc_count);
    m->syn_dirty=0;
}
const Synonyms *tilemem_synonyms(const TileMemory *m){ return m->syn; }

/* ---- search (HOT + WARM stream), value copies in m->res ---- */
static const char *res_add(TileMemory *m, const char *v){
    if(m->n_res==m->cap_res){ m->cap_res=m->cap_res?m->cap_res*2:32; m->res=(char**)realloc(m->res,(size_t)m->cap_res*sizeof(char*)); }
    m->res[m->n_res]=strdup(v?v:""); return m->res[m->n_res++];
}
static void topk_insert(TileMemory *m, TileHit *out, int *n, int lim, float score,
                        const char *id, const char *label, const char *source, const char *value){
    if(score<=0) return;
    if(*n>=lim && score<=out[lim-1].score) return;
    int pos=(*n<lim)? *n : lim-1;
    if(*n<lim) (*n)++;
    int j=pos; while(j>0 && out[j-1].score<score){ out[j]=out[j-1]; j--; }
    snprintf(out[j].id,sizeof(out[j].id),"%s",id?id:"");
    snprintf(out[j].label,sizeof(out[j].label),"%s",label?label:"");
    snprintf(out[j].source,sizeof(out[j].source),"%s",source?source:"");
    out[j].value=res_add(m, value); out[j].score=score;
}
/* Build the weighted query-term set (hard idf terms + optional PPMI soft terms).
   qt[] (caller-owned) backs the hard-term strings; soft terms point into the synonym map.
   Returns the number of weighted terms written to wq/ww (<= cap). Shared by both searchers. */
static int build_weighted_query(TileMemory *m, const char *query, char qt[][32],
                                const char **wq, double *ww, int cap){
    int nq=tokenize_terms(query,qt,64);
    int qidx[64]; int nqd=0;
    for(int a=0;a<nq && nqd<64;a++){ int dup=0; for(int b=0;b<nqd;b++) if(strcmp(qt[a],qt[qidx[b]])==0){ dup=1; break; }
        if(!dup) qidx[nqd++]=a; }
    int nw=0;
    for(int q=0;q<nqd && nw<cap;q++){ unsigned df=0; unsigned *d=tdf_slot(&m->tdf,qt[qidx[q]],0); if(d) df=*d;
        wq[nw]=qt[qidx[q]]; ww[nw]=log(((double)m->doc_count+1.0)/((double)df+1.0))+1.0; nw++; }
    if(m->syn_alpha>0.0 && m->syn){
        if(m->syn_dirty) tilemem_build_synonyms(m);
        int base=nw;
        for(int q=0;q<nqd && nw<cap;q++){
            const char *nb[16]; float pp[16]; int kk=m->syn_k<16? m->syn_k:16;
            int got=syn_neighbors(m->syn,qt[qidx[q]],nb,pp,kk);
            for(int z=0;z<got && nw<cap;z++){
                if(nw-base>=m->syn_max_expand) break;
                int dup=0; for(int w=0;w<nw;w++) if(strcmp(wq[w],nb[z])==0){ dup=1; break; }
                if(dup) continue;
                unsigned df=0; unsigned *d=tdf_slot(&m->tdf,(char*)nb[z],0); if(d) df=*d;
                double idf=log(((double)m->doc_count+1.0)/((double)df+1.0))+1.0;
                double scl=(double)pp[z]/m->syn_pmi_scale; if(scl>1.0) scl=1.0;
                wq[nw]=nb[z]; ww[nw]=idf*scl*m->syn_alpha; nw++;
            }
        }
    }
    return nw;
}
/* TF-IDF overlap score for one tile key over the weighted query terms, length-normalized.
   Pure (no side effects) so HOT and WARM, oracle and index, all score identically. */
static float score_tile(const char *key, const char **wq, const double *ww, int nw){
    char tt[256][32]; int nt=tokenize_terms(key,tt,256);
    double s=0; char hit[128]={0};
    for(int k=0;k<nt;k++) for(int w=0;w<nw;w++) if(!hit[w] && strcmp(tt[k],wq[w])==0){ s+=ww[w]; hit[w]=1; break; }
    return (float)(s/sqrt((double)nt+1.0));
}
int tilemem_search_linear(TileMemory *m, const char *query, int topK, TileHit *out, int cap){
    for(int i=0;i<m->n_res;i++) free(m->res[i]);
    m->n_res=0;
    char qt[64][32]; const char *wq[128]; double ww[128];
    int nw=build_weighted_query(m,query,qt,wq,ww,128);
    int lim=topK<cap?topK:cap; int n=0;
    for(int i=0;i<m->n_hot;i++){
        float sc=score_tile(m->hot[i].key,wq,ww,nw);
        if(sc>0){ topk_insert(m,out,&n,lim,sc,m->hot[i].id,m->hot[i].label,m->hot[i].source,m->hot[i].value); m->hot[i].heat++; }
    }
    char wp[300]; warm_path(m,wp,sizeof(wp)); FILE *f=fopen(wp,"rb");
    if(f){ Tile t; while(tile_read(f,&t,m->dim)==0){
        float sc=score_tile(t.key,wq,ww,nw);
        topk_insert(m,out,&n,lim,sc,t.id,t.label,t.source,t.value);
        tile_free(&t); } fclose(f); }
    return n;
}
int tilemem_search(TileMemory *m, const char *query, int topK, TileHit *out, int cap){
    for(int i=0;i<m->n_res;i++) free(m->res[i]);
    m->n_res=0;
    char qt[64][32]; const char *wq[128]; double ww[128];
    int nw=build_weighted_query(m,query,qt,wq,ww,128);
    int lim=topK<cap?topK:cap; int n=0;
    /* gather candidate tids from postings; collect WARM candidates' offsets */
    m->seen_epoch++; seen_ensure(m);
    size_t *wc=NULL; int wcn=0, wccap=0;
    for(int w=0;w<nw;w++){ int pn; unsigned *pl=tdf_postings(&m->tdf,wq[w],&pn);
        for(int p=0;p<pn;p++){ unsigned tid=pl[p];
            if(!loc_live(m,tid)) continue;
            if(m->seen[tid]==m->seen_epoch) continue;
            m->seen[tid]=m->seen_epoch;
            if(m->loc[tid].tier==1){ if(wcn==wccap){ wccap=wccap?wccap*2:16;
                    wc=(size_t*)realloc(wc,(size_t)wccap*sizeof(size_t)); } wc[wcn++]=m->loc[tid].where; }
        } }
    /* HOT pass in array order (preserves tie-break order); score candidates only */
    for(int i=0;i<m->n_hot;i++){
        if(m->seen[m->hot[i].tid]!=m->seen_epoch) continue;
        float sc=score_tile(m->hot[i].key,wq,ww,nw);
        if(sc>0){ topk_insert(m,out,&n,lim,sc,m->hot[i].id,m->hot[i].label,m->hot[i].source,m->hot[i].value); m->hot[i].heat++; }
    }
    /* WARM pass by ascending offset (= file order); read only candidate tiles */
    if(wcn>0){ qsort(wc,(size_t)wcn,sizeof(size_t),cmp_size);
        char wp[300]; warm_path(m,wp,sizeof(wp)); FILE *f=fopen(wp,"rb");
        if(f){ for(int c=0;c<wcn;c++){ if(fseek(f,(long)wc[c],SEEK_SET)!=0) break; Tile t;
            if(tile_read(f,&t,m->dim)!=0) break;
            float sc=score_tile(t.key,wq,ww,nw);
            topk_insert(m,out,&n,lim,sc,t.id,t.label,t.source,t.value);
            tile_free(&t); } fclose(f); } }
    free(wc);
    return n;
}

void tilemem_set_expansion(TileMemory *m, double alpha, int k, int min_cooc,
                           int min_df, double df_frac, int max_expand){
    m->syn_alpha=alpha;
    if(max_expand>0) m->syn_max_expand=max_expand;
    /* Only build-affecting params invalidate the map; alpha/max_expand are search-time. */
    if(k>0 && k!=m->syn_k){ m->syn_k=k; m->syn_dirty=1; }
    if(min_cooc>0 && min_cooc!=m->syn_min_cooc){ m->syn_min_cooc=min_cooc; m->syn_dirty=1; }
    if(min_df>0 && min_df!=m->syn_min_df){ m->syn_min_df=min_df; m->syn_dirty=1; }
    if(df_frac>0.0 && df_frac!=m->syn_df_frac){ m->syn_df_frac=df_frac; m->syn_dirty=1; }
}

void tilemem_set_discount(TileMemory *m, int on){ m->syn_discount = on?1:0; m->syn_dirty=1; }

void tilemem_decay(TileMemory *m){ for(int i=0;i<m->n_hot;i++) if(m->hot[i].heat>0) m->hot[i].heat--; }
size_t tilemem_hot_count(const TileMemory *m){ return (size_t)m->n_hot; }
size_t tilemem_warm_count(const TileMemory *m){ return m->warm_count; }
size_t tilemem_total(const TileMemory *m){ return (size_t)m->n_hot + m->warm_count; }
size_t tilemem_certifiable(const TileMemory *m, int min_count, double min_share){
    (void)min_share; size_t c=0; for(int i=0;i<m->n_hot;i++) if(m->hot[i].count>=min_count) c++; return c;
}
size_t tilemem_evict_containing(TileMemory *m, const char *needle){
    if(!needle||!*needle) return 0;
    size_t ev=0;
    for(int i=0;i<m->n_hot;){
        if(strstr(m->hot[i].key, needle)){ remove_hot_by_tid(m, m->hot[i].tid); ev++; }   /* swaps last into i */
        else i++;
    }
    return ev;
}

/* ---- Lever 6: opt-in semantic consolidation (merge paraphrase tiles via the PPMI map) ---- */
static int term_in(const char *t, const char **ty, int ny){
    for(int i=0;i<ny;i++) if(strcmp(t,ty[i])==0) return 1;
    return 0;
}
/* fraction of TX terms covered by TY: exact match, or a top-k PPMI neighbor of the term is in TY */
static double coverage(TileMemory *m, const char **tx, int nx, const char **ty, int ny){
    if(nx==0) return 0.0;
    int c=0;
    for(int i=0;i<nx;i++){
        if(term_in(tx[i],ty,ny)){ c++; continue; }
        const char *nb[16]; float pp[16]; int kk=m->syn_k<16? m->syn_k:16;
        int got=m->syn? syn_neighbors(m->syn,tx[i],nb,pp,kk):0;
        int hit=0; for(int z=0;z<got;z++) if(term_in(nb[z],ty,ny)){ hit=1; break; }
        if(hit) c++;
    }
    return (double)c/(double)nx;
}

size_t tilemem_consolidate(TileMemory *m, double tau_sem, int min_terms, double max_df_frac, int max_cand, ConsolidateReport *rep){
    if(min_terms<1) min_terms=1;
    clock_t t0=clock();
    if(m->syn_dirty) tilemem_build_synonyms(m);     /* ensure the PPMI map exists */
    size_t before=(size_t)m->n_hot + m->warm_count;
    /* snapshot live HOT tids so the outer loop is stable while tiles are removed */
    int ns=m->n_hot;
    unsigned *tids=(unsigned*)malloc((size_t)(ns>0?ns:1)*sizeof(unsigned));
    for(int i=0;i<m->n_hot;i++) tids[i]=m->hot[i].tid;
    size_t merges=0;
    size_t comparisons=0;
    double df_cut = (max_df_frac>0.0 && max_df_frac<1.0) ? max_df_frac*(double)m->doc_count : 0.0;
    for(int s=0;s<ns;s++){
        unsigned at=tids[s];
        if(!loc_live(m,at) || m->loc[at].tier!=0) continue;   /* already merged away */
        char ta_store[256][32]; const char *TA[256];
        int na=tile_distinct_terms(m->hot[(int)m->loc[at].where].key, ta_store, TA, 256);
        if(na<min_terms) continue;
        /* order terms rarest-first (ascending df): the cap then keeps the most discriminative candidates */
        int order[256]; for(int i=0;i<na;i++) order[i]=i;
        for(int a=1;a<na;a++){ int v=order[a];
            unsigned *dva=tdf_slot(&m->tdf,TA[v],0); unsigned da=dva? *dva:0;
            int b=a-1;
            while(b>=0){ unsigned *dvb=tdf_slot(&m->tdf,TA[order[b]],0); unsigned db=dvb? *dvb:0;
                if(db<=da) break;
                order[b+1]=order[b]; b--; }
            order[b+1]=v; }
        int cap = (max_cand>0)? max_cand : (1<<30);   /* large = unbounded */
        /* gather candidate tids via postings of TA's terms AND their PPMI neighbors, rarest-first, capped */
        m->seen_epoch++; seen_ensure(m);
        unsigned *cand=NULL; int cn=0, cc=0;
        for(int oi=0; oi<na && cn<cap; oi++){
            int i=order[oi];
            if(df_cut>0.0 && m->doc_count>0){
                unsigned *dfp=tdf_slot(&m->tdf,TA[i],0); unsigned dft=dfp? *dfp:0;
                if((double)dft > df_cut) continue;   /* skip a common term in candidate gathering */
            }
            for(int pass=0;pass<2 && cn<cap;pass++){
                int pn=0; unsigned *pl=NULL;
                if(pass==0){ pl=tdf_postings(&m->tdf,TA[i],&pn); }
                else { const char *nb[16]; float pp[16]; int kk=m->syn_k<16? m->syn_k:16;
                       int got=m->syn? syn_neighbors(m->syn,TA[i],nb,pp,kk):0;
                       for(int z=0;z<got && cn<cap;z++){ int qn; unsigned *ql=tdf_postings(&m->tdf,nb[z],&qn);
                           for(int q=0;q<qn && cn<cap;q++){ unsigned tid=ql[q];
                               if(tid==at||!loc_live(m,tid)||m->loc[tid].tier!=0) continue;
                               if(m->seen[tid]==m->seen_epoch) continue;
                               m->seen[tid]=m->seen_epoch;
                               if(cn==cc){ cc=cc?cc*2:16; cand=(unsigned*)realloc(cand,(size_t)cc*sizeof(unsigned)); }
                               cand[cn++]=tid; } }
                       continue; }
                for(int p=0;p<pn && cn<cap;p++){ unsigned tid=pl[p];
                    if(tid==at||!loc_live(m,tid)||m->loc[tid].tier!=0) continue;
                    if(m->seen[tid]==m->seen_epoch) continue;
                    m->seen[tid]=m->seen_epoch;
                    if(cn==cc){ cc=cc?cc*2:16; cand=(unsigned*)realloc(cand,(size_t)cc*sizeof(unsigned)); }
                    cand[cn++]=tid; }
            }
        }
        for(int ci=0; ci<cn; ci++){
            unsigned ct=cand[ci];
            if(!loc_live(m,ct) || m->loc[ct].tier!=0) continue;
            char tc_store[256][32]; const char *TC[256];
            int nc=tile_distinct_terms(m->hot[(int)m->loc[ct].where].key, tc_store, TC, 256);
            if(nc<min_terms) continue;
            comparisons++;
            double cab=coverage(m,TA,na,TC,nc), cba=coverage(m,TC,nc,TA,na);
            double sim=cab<cba?cab:cba;
            if(sim>=tau_sem){
                Tile *A=&m->hot[(int)m->loc[at].where];
                Tile *C=&m->hot[(int)m->loc[ct].where];
                if(A->count>=C->count){ A->count+=C->count; if(C->heat>A->heat) A->heat=C->heat; remove_hot_by_tid(m,ct); }
                else { C->count+=A->count; if(A->heat>C->heat) C->heat=A->heat; remove_hot_by_tid(m,at); }
                merges++;
                if(!loc_live(m,at)) break;   /* A itself was absorbed -> done with its candidates */
                /* TA still valid (tokenized into ta_store; A only moved in the array, not freed) */
            }
        }
        free(cand);
    }
    free(tids);
    if(rep){ rep->tiles_before=before; rep->tiles_after=(size_t)m->n_hot+m->warm_count;
             rep->merges=merges; rep->comparisons=comparisons;
             rep->ms=1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC; }
    return merges;
}
