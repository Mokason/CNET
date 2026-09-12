/* Research harness only: no production routing policy or capsule changes. */
#define _POSIX_C_SOURCE 200809L
#include "cnet_vsa_gen_capsule.h"
#include "cnet_vsa_reason.h"
#include "cnet_vsa_text.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define ENT 128
#define DEPTH 8
#define REL 8
#define FACT 512
#define DIM 2048

typedef struct { int s, r, o, sign; } Edge;
typedef struct {
    int n, d, nr, nf;
    Edge edges[FACT];
    int8_t *subject, *object;
    int16_t *memory;
    CnetVsaGenRegistry *reg;
    double encode_us, topical_build_us;
} World;
typedef struct { int answer, iterations, status; double us; } Answer;
uint32_t experiment_default_encoder(void) { return CNET_VSA_ENCODER_DEFAULT; }
static double clock_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}
static uint64_t random64(uint64_t *x) { *x ^= *x << 13; *x ^= *x >> 7; *x ^= *x << 17; return *x; }
static void signs(int8_t *v, int d, uint64_t *seed) {
    for (int i=0; i<d; ++i) v[i]=(random64(seed)&1) ? 1 : -1;
}
void *world_create(int n, int d, int nr, uint64_t seed, const Edge *edges, int nf) {
    if(n<1||n>ENT||d<1||d>DIM||nr<1||nr>REL||nf<0||nf>FACT) return NULL;
    World *w=calloc(1,sizeof(*w)); if(!w) return NULL;
    w->n=n; w->d=d; w->nr=nr; w->nf=nf;
    w->subject=malloc((size_t)n*d); w->object=malloc((size_t)n*d);
    w->memory=calloc((size_t)nr*d,sizeof(int16_t)); w->reg=calloc(1,sizeof(*w->reg));
    if(!w->subject||!w->object||!w->memory||!w->reg) abort();
    memcpy(w->edges,edges,(size_t)nf*sizeof(Edge));
    double t=clock_us();
    signs(w->subject,n*d,&seed); signs(w->object,n*d,&seed);
    for(int i=0;i<nf;++i) {
        Edge e=edges[i];
        if(e.s<0||e.s>=n||e.o<0||e.o>=n||e.r<0||e.r>=nr) abort();
        /* Negatives stay explicit for validation; cancellation must not erase a contradiction. */
        if(e.sign<0) continue;
        for(int k=0;k<d;++k) w->memory[e.r*d+k]+=w->subject[e.s*d+k]*w->object[e.o*d+k];
    }
    w->encode_us=clock_us()-t; t=clock_us();
    cnet_vsa_registry_init(w->reg,CNET_VSA_DEFAULT_DIM);
    for(int i=0;i<nf;++i) {
        Edge e=edges[i]; CnetVsaRegisteredCap *c=&w->reg->capsules[w->reg->count++];
        char text[128]; snprintf(text,sizeof text,"node%03d %srel%02d node%03d",e.s,e.sign<0?"not ":"",e.r,e.o);
        c->header.certified=1; c->header.safe_radius=.9f; c->has_topical=1;
        c->topical_radius=.9f; c->encoder_id=CNET_VSA_ENCODER_STEM;
        if(cnet_vsa_gencap_encode_intent_q8(text,c->topical,c->encoder_id)) abort();
        c->topical_norm=cnet_vsa_text_q8_norm(c->topical);
    }
    w->topical_build_us=clock_us()-t;
    return w;
}
void world_free(void *ptr) { World*w=ptr; if(w){free(w->subject);free(w->object);free(w->memory);free(w->reg);free(w);} }
void world_costs(void *ptr, double *out) {
    World*w=ptr; out[0]=w->encode_us;out[1]=w->topical_build_us;
    out[2]=(double)(2*w->n*w->d+2*w->nr*w->d); out[3]=sizeof(*w->reg);
    out[4]=(double)w->nf*sizeof(Edge); out[5]=sizeof(*w);
}
/* Independent C implementation, cross-checked against the Python fixture oracle.
 * A contradiction on ANY reachable positive edge refuses the whole query. */
static int exact(World*w,int start,const int*rels,int depth) {
    unsigned char active[ENT]={0}, next[ENT]; active[start]=1;
    for(int h=0;h<depth;++h) {
        memset(next,0,sizeof next);
        for(int i=0;i<w->nf;++i) {
            Edge e=w->edges[i]; if(e.sign<0||!active[e.s]||e.r!=rels[h]) continue;
            for(int j=0;j<w->nf;++j) {
                Edge f=w->edges[j]; if(f.sign<0&&f.s==e.s&&f.r==e.r&&f.o==e.o) return -1;
            }
            next[e.o]=1;
        }
        memcpy(active,next,sizeof active);
    }
    int answer=-1;
    for(int e=0;e<w->n;++e) if(active[e]) {if(answer>=0)return -1;answer=e;}
    return answer;
}
static int direct(World*w,int start,const int*rels,int depth) {
    int at=start;
    for(int h=0;h<depth;++h) {
        int best=-1; int32_t maximum=INT32_MIN;
        for(int e=0;e<w->n;++e) {
            int32_t sum=0;
            for(int k=0;k<w->d;++k) sum+=(int32_t)w->memory[rels[h]*w->d+k]*w->subject[at*w->d+k]*w->object[e*w->d+k];
            if(sum>maximum){maximum=sum;best=e;}
        }
        at=best;
    }
    return at;
}
/* Experimental synchronous factor-graph updates, NOT the production factor3
 * resonator. Preserve superposed entity candidates at every unknown chain node. */
static int coupled(World*w,int start,const int*rels,int depth,int cycles) {
    float belief[DEPTH+1][ENT]={{0}}, next[DEPTH+1][ENT]={{0}};
    float fwd[DIM],back[DIM],score[ENT];
    belief[0][start]=1; next[0][start]=1;
    for(int h=1;h<=depth;++h)for(int e=0;e<w->n;++e)belief[h][e]=1.0f/w->n;
    for(int t=0;t<cycles;++t) {
        for(int h=1;h<=depth;++h) {
            memset(fwd,0,(size_t)w->d*sizeof(float));memset(back,0,(size_t)w->d*sizeof(float));
            for(int e=0;e<w->n;++e)for(int k=0;k<w->d;++k) {
                fwd[k]+=belief[h-1][e]*w->subject[e*w->d+k];
                if(h<depth)back[k]+=belief[h+1][e]*w->object[e*w->d+k];
            }
            float maximum=-INFINITY;
            for(int e=0;e<w->n;++e) {
                float s=0;
                for(int k=0;k<w->d;++k) {
                    s+=w->memory[rels[h-1]*w->d+k]*fwd[k]*w->object[e*w->d+k];
                    if(h<depth)s+=.5f*w->memory[rels[h]*w->d+k]*back[k]*w->subject[e*w->d+k];
                }
                score[e]=s/w->d;if(score[e]>maximum)maximum=score[e];
            }
            float z=0;
            for(int e=0;e<w->n;++e){next[h][e]=expf(4*(score[e]-maximum));z+=next[h][e];}
            for(int e=0;e<w->n;++e)next[h][e]/=z;
        }
        memcpy(belief,next,sizeof belief);
    }
    int best=0;for(int e=1;e<w->n;++e)if(belief[depth][e]>belief[depth][best])best=e;
    return best;
}
void world_query(void *ptr,int start,const int*rels,int depth,int lane,int cycles,int verified,Answer*out) {
    World*w=ptr; double t=clock_us();int answer=-1;
    if(depth<1||depth>DEPTH||start<0||start>=w->n)abort();
    if(lane==0)answer=exact(w,start,rels,depth);
    else if(lane==1) {
        char prompt[256];int off=snprintf(prompt,sizeof prompt,"node%03d",start);
        for(int i=0;i<depth;++i)off+=snprintf(prompt+off,sizeof prompt-(size_t)off," rel%02d",rels[i]);
        CnetVsaRouteResult rr;int i=cnet_vsa_registry_route_query(w->reg,prompt,&rr);
        if(i>=0)answer=w->edges[i].o;
    } else if(lane==2)answer=direct(w,start,rels,depth);
    else if(lane==3)answer=coupled(w,start,rels,depth,cycles);
    if(verified && answer!=exact(w,start,rels,depth))answer=-1;
    out->answer=answer;out->iterations=lane==3?cycles:(lane==2?depth:0);
    out->status=answer<0?1:0;out->us=clock_us()-t;
}

typedef struct { int x,y,z,iterations,raw_accept,verified_accept; double us,build_us; size_t bytes; float reconstruction; } FactorResult;
/* Generate the same deterministic books/target for every iteration budget.
 * lane 0: production float with native bipolar RNG; 1: production float with paired bipolar keys;
 * lane 2: experimental bipolar sign + outer-product resonator. */
void factor_trial(int lane,int d,int count,uint64_t seed,int x,int y,int z,int absent,float noise,int cap,FactorResult*out) {
    memset(out,0,sizeof(*out));
    if(d>DIM||d<1||count<1||count>ENT||(lane<2&&d>512))abort();
    double begin=clock_us();
    float *book[3];int8_t *bits[3]; CnetVsaCodebook cb[3];
    for(int f=0;f<3;++f) {
        book[f]=malloc((size_t)count*d*sizeof(float));bits[f]=malloc((size_t)count*d);
        signs(bits[f],count*d,&seed);
        if(lane==0)for(int i=0;i<count;++i)cnet_vsa_random(book[f]+i*d,d,&seed);
        else for(int i=0;i<count*d;++i)book[f][i]=bits[f][i]/sqrtf((float)d);
        if(lane<2){cnet_vsa_codebook_init(&cb[f],d,(size_t)count);for(int i=0;i<count;++i){char name[64];snprintf(name,sizeof name,"%d",i);cnet_vsa_codebook_add(&cb[f],name,book[f]+i*d);}}
    }
    float composite[DIM],tmp[DIM],novel[DIM]; int8_t target[DIM];
    if(absent){if(lane==0)cnet_vsa_random(novel,d,&seed);else for(int k=0;k<d;++k)novel[k]=(random64(&seed)&1?1:-1)/sqrtf((float)d);}
    cnet_vsa_bind(tmp,absent?novel:book[0]+x*d,book[1]+y*d,d);
    cnet_vsa_bind(composite,tmp,book[2]+z*d,d);
    for(int k=0;k<d;++k) {
        if((double)(random64(&seed)%1000000)/1000000.0<noise)composite[k]=-composite[k];
        target[k]=composite[k]>=0?1:-1;
    }
    out->build_us=clock_us()-begin;begin=clock_us();int ids[3]={0};
    if(lane<2) {
        CnetVsaResonatorResult r;
        cnet_vsa_resonator_factor3(composite,&cb[0],&cb[1],&cb[2],cap,&r);
        ids[0]=atoi(r.name_x);ids[1]=atoi(r.name_y);ids[2]=atoi(r.name_z);
        out->iterations=r.iterations;out->raw_accept=r.converged;
    } else {
        int8_t est[3][DIM];float probe[DIM],projection[DIM];
        for(int f=0;f<3;++f)for(int k=0;k<d;++k){int v=0;for(int i=0;i<count;++i)v+=bits[f][i*d+k];est[f][k]=v>=0?1:-1;}
        for(int t=0;t<cap;++t) {
            for(int f=0;f<3;++f) {
                for(int k=0;k<d;++k)probe[k]=target[k]*est[(f+1)%3][k]*est[(f+2)%3][k];
                memset(projection,0,(size_t)d*sizeof(float));
                for(int i=0;i<count;++i) {
                    float dot=0;for(int k=0;k<d;++k)dot+=probe[k]*bits[f][i*d+k];
                    for(int k=0;k<d;++k)projection[k]+=dot*bits[f][i*d+k];
                }
                for(int k=0;k<d;++k)est[f][k]=projection[k]>=0?1:-1;
            }
            for(int f=0;f<3;++f) {
                int best=INT32_MIN;
                for(int i=0;i<count;++i){int dot=0;for(int k=0;k<d;++k)dot+=est[f][k]*bits[f][i*d+k];if(dot>best){best=dot;ids[f]=i;}}
            }
            int dot=0;for(int k=0;k<d;++k)dot+=target[k]*bits[0][ids[0]*d+k]*bits[1][ids[1]*d+k]*bits[2][ids[2]*d+k];
            out->iterations=t+1;
            if((float)dot/d>=.75f){out->raw_accept=1;break;}
        }
    }
    cnet_vsa_bind(tmp,book[0]+ids[0]*d,book[1]+ids[1]*d,d);
    cnet_vsa_bind(novel,tmp,book[2]+ids[2]*d,d);
    out->reconstruction=cnet_vsa_similarity(composite,novel,d);
    out->verified_accept=out->raw_accept && out->reconstruction>=.75f;
    out->us=clock_us()-begin;
    out->x=ids[0];out->y=ids[1];out->z=ids[2];
    /* Solver book storage, excluding experiment-only duplicate generation buffers. */
    out->bytes=(size_t)3*count*d*(lane<2?sizeof(float):sizeof(int8_t));
    if(lane<2)out->bytes+=(size_t)3*count*CNET_VSA_NAME_MAX;
    for(int f=0;f<3;++f){if(lane<2)cnet_vsa_codebook_free(&cb[f]);free(book[f]);free(bits[f]);}
}
