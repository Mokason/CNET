/* Graduation: mine a near-deterministic regularity from the corpus, distill it into
   two frozen btn_certify-proven positionally-tagged units, register them certified,
   and evict the source tiles from the fuzzy memory. Closes the soft->hard loop. */
#include "../../include/corpus/graduate.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* --- local string->id vocab over the whole corpus --- */
typedef struct { char **w; int n, cap; } Vocab;
static int v_id(Vocab *v, const char *s, int add){
    for(int i=0;i<v->n;i++) if(strcmp(v->w[i],s)==0) return i;
    if(!add) return -1;
    if(v->n==v->cap){ v->cap=v->cap?v->cap*2:256; v->w=(char**)realloc(v->w,(size_t)v->cap*sizeof(char*)); }
    v->w[v->n]=strdup(s); return v->n++;
}
static int v_tok(Vocab *v, const char *s, int *out, int cap){ int n=0; char cur[64]; int cl=0;
    for(const char *p=s;;p++){ char c=*p; int al=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='\'';
        if(al){ if(cl<63) cur[cl++]=(c>='A'&&c<='Z')?(char)(c+32):c; }
        else { if(cl>0){ cur[cl]=0; int id=v_id(v,cur,1); if(id>=0&&n<cap) out[n++]=id; cl=0; }
               if(c=='.'||c=='!'||c=='?'){ int id=v_id(v,".",1); if(id>=0&&n<cap) out[n++]=id; }
               if(c==0) break; } }
    return n;
}
/* per-word continuation counts */
typedef struct { int *nx,*cn,n,cap; } Conts;
static void c_add(Conts *c, int nx){
    for(int i=0;i<c->n;i++) if(c->nx[i]==nx){ c->cn[i]++; return; }
    if(c->n==c->cap){ c->cap=c->cap?c->cap*2:4; c->nx=(int*)realloc(c->nx,(size_t)c->cap*sizeof(int)); c->cn=(int*)realloc(c->cn,(size_t)c->cap*sizeof(int)); }
    c->nx[c->n]=nx; c->cn[c->n]=1; c->n++;
}

void graduated_units_free(GraduatedUnits *gu){
    if(!gu||!gu->valid) return;
    contract_free(&gu->con_a); contract_free(&gu->con_b);
    btn_free(&gu->btn_a); btn_free(&gu->btn_b);
    free(gu->X); free(gu->Y); gu->valid=0;
}

int graduate_deterministic(const StrList *corpus, PrimitiveRegistry *reg, TileMemory *mem,
                           int min_count, double min_share, int max_sources,
                           GraduatedUnits *gu, GraduateReport *rep){
    memset(rep,0,sizeof(*rep)); memset(gu,0,sizeof(*gu));
    clock_t t0=clock();

    /* 1) tokenize whole corpus, count bigrams (per-word continuation lists) */
    Vocab voc={0};
    Conts *cont=NULL; int vcap=0;
    for(size_t s=0;s<corpus->count;s++){
        int t[2048]; int nt=v_tok(&voc,corpus->lines[s],t,2048);
        if(voc.n>vcap){ int old=vcap; vcap=voc.n; cont=(Conts*)realloc(cont,(size_t)vcap*sizeof(Conts));
            for(int i=old;i<vcap;i++){ cont[i].nx=NULL; cont[i].cn=NULL; cont[i].n=0; cont[i].cap=0; } }
        int prev=-1;
        for(int i=0;i<nt;i++){
            if(prev>=0){
                if(voc.n>vcap){ int old=vcap; vcap=voc.n; cont=(Conts*)realloc(cont,(size_t)vcap*sizeof(Conts));
                    for(int k=old;k<vcap;k++){ cont[k].nx=NULL; cont[k].cn=NULL; cont[k].n=0; cont[k].cap=0; } }
                c_add(&cont[prev], t[i]);
            }
            prev=t[i];
        }
    }
    if(voc.n<1){   /* empty corpus -> nothing to mine (also bounds the alloc sizes) */
        for(int i=0;i<vcap;i++){ free(cont[i].nx); free(cont[i].cn); }
        free(cont);
        for(int i=0;i<voc.n;i++) free(voc.w[i]);
        free(voc.w);
        return -1;
    }
    /* 2) sources: dominant share >= min_share, total >= min_count. dom[w]=best next. */
    int *dom=(int*)malloc((size_t)voc.n*sizeof(int));
    int *issrc=(int*)calloc((size_t)voc.n,sizeof(int));
    int *srctot=(int*)calloc((size_t)voc.n,sizeof(int));
    for(int w=0;w<voc.n;w++){
        dom[w]=-1;
        if(w>=vcap) continue;
        int tot=0,best=-1,bc=0;
        for(int i=0;i<cont[w].n;i++){ tot+=cont[w].cn[i]; if(cont[w].cn[i]>bc){ bc=cont[w].cn[i]; best=cont[w].nx[i]; } }
        dom[w]=best;
        if(best>=0 && tot>=min_count && (double)bc/tot>=min_share){ issrc[w]=1; srctot[w]=tot; }
    }
    /* keep only the top max_sources sources by total count */
    int *keep=(int*)calloc((size_t)voc.n,sizeof(int)); int nkeep=0;
    for(int pass=0; pass<max_sources; pass++){ int b=-1;
        for(int w=0;w<voc.n;w++) if(issrc[w]&&!keep[w]&&(b<0||srctot[w]>srctot[b])) b=w;
        if(b<0) break;
        keep[b]=1; nkeep++;
    }
    rep->ms_mine = 1000.0*(double)(clock()-t0)/CLOCKS_PER_SEC;

    /* 3) scoped vocab = kept sources + their targets; map to [0,V) */
    int *scoped=(int*)malloc((size_t)voc.n*sizeof(int)); for(int i=0;i<voc.n;i++) scoped[i]=-1;
    int V=0;
    for(int w=0;w<voc.n;w++) if(keep[w]){ if(scoped[w]<0) scoped[w]=V++; int d=dom[w]; if(d>=0 && scoped[d]<0) scoped[d]=V++; }
    if(V<2){
        free(dom);free(issrc);free(srctot);free(keep);free(scoped);
        for(int i=0;i<vcap;i++){ free(cont[i].nx); free(cont[i].cn); }
        free(cont);
        for(int i=0;i<voc.n;i++) free(voc.w[i]);
        free(voc.w);
        return -1;
    }
    /* find a seed X (kept source) whose Y=dom(X) is also a kept source -> 2-step run */
    int seedw=-1;
    for(int w=0;w<voc.n;w++) if(keep[w] && dom[w]>=0 && keep[dom[w]]){ seedw=w; break; }

    /* 4) exemplar tables over scoped vocab: each kept source -> its dom (one-hot) */
    int E=0; for(int w=0;w<voc.n;w++) if(keep[w]) E++;
    double *Xt=(double*)calloc((size_t)E*V,sizeof(double));
    double *Yt=(double*)calloc((size_t)E*V,sizeof(double));
    { int r=0; for(int w=0;w<voc.n;w++) if(keep[w]){ Xt[(size_t)r*V+scoped[w]]=1.0; Yt[(size_t)r*V+scoped[dom[w]]]=1.0; r++; } }
    rep->exemplars=E; rep->vocab=V;

    /* 5) build + certify the two positionally tagged units (same map, distinct tags) */
    clock_t tb=clock();
    snprintf(gu->tag_a,sizeof(gu->tag_a),"w_a"); snprintf(gu->tag_b,sizeof(gu->tag_b),"w_b"); snprintf(gu->tag_c,sizeof(gu->tag_c),"w_c");
    int hid = V*2; if(hid<64) hid=64; if(hid>512) hid=512;
    int mxh = V*4; if(mxh<256) mxh=256; if(mxh>1024) mxh=1024;
    btn_init(&gu->btn_a,(size_t)V,(size_t)V,(size_t)hid,(size_t)mxh,0.08,1234u);
    btn_init(&gu->btn_b,(size_t)V,(size_t)V,(size_t)hid,(size_t)mxh,0.08,1234u);
    Port ia={PORT_ONEHOT,(size_t)V,1,""}; port_set_tag(&ia,gu->tag_a);
    Port ob={PORT_ONEHOT,(size_t)V,1,""}; port_set_tag(&ob,gu->tag_b);
    Port ib={PORT_ONEHOT,(size_t)V,1,""}; port_set_tag(&ib,gu->tag_b);
    Port oc={PORT_ONEHOT,(size_t)V,1,""}; port_set_tag(&oc,gu->tag_c);
    btn_set_ports(&gu->btn_a,ia,ob);
    btn_set_ports(&gu->btn_b,ib,oc);
    btn_train(&gu->btn_a,Xt,Yt,(size_t)E,6000);
    btn_train(&gu->btn_b,Xt,Yt,(size_t)E,6000);
    contract_init_borrowed(&gu->con_a,"step_ab",&gu->btn_a,Xt,Yt,(size_t)E);
    contract_init_borrowed(&gu->con_b,"step_bc",&gu->btn_b,Xt,Yt,(size_t)E);
    CertifyReport cra,crb; memset(&cra,0,sizeof(cra)); memset(&crb,0,sizeof(crb));
    rep->cert_a = btn_certify(&gu->btn_a,&gu->con_a,&cra);
    rep->cert_b = btn_certify(&gu->btn_b,&gu->con_b,&crb);
    gu->X=Xt; gu->Y=Yt; gu->vocab=V; gu->valid=1;
    rep->ms_build_certify = 1000.0*(double)(clock()-tb)/CLOCKS_PER_SEC;

    /* 6) register certified */
    clock_t tr=clock();
    reg->require_certified=1;
    int ra=registry_add_certified(reg,&gu->btn_a,"step_ab",&gu->con_a);
    int rb=registry_add_certified(reg,&gu->btn_b,"step_bc",&gu->con_b);
    rep->units=(ra==0)+(rb==0);
    rep->ms_register = 1000.0*(double)(clock()-tr)/CLOCKS_PER_SEC;

    /* 7) evict the seed run's tiles from fuzzy memory */
    clock_t te=clock();
    rep->mem_before = mem? tilemem_total(mem):0;
    if(seedw>=0){
        snprintf(rep->seed,sizeof(rep->seed),"%s",voc.w[seedw]);
        snprintf(rep->mid,sizeof(rep->mid),"%s",voc.w[dom[seedw]]);
        snprintf(rep->dst,sizeof(rep->dst),"%s",voc.w[dom[dom[seedw]]]);
        gu->seed_idx=scoped[seedw]; gu->dst_idx=scoped[dom[dom[seedw]]];
        rep->seed_idx=gu->seed_idx; rep->dst_idx=gu->dst_idx;
        if(mem) rep->tiles_evicted = tilemem_evict_containing(mem, rep->seed);
        rep->ok = (rep->cert_a==0 && rep->cert_b==0 && rep->units==2) ? 1 : 0;
    }
    rep->mem_after = mem? tilemem_total(mem):0;
    rep->ms_evict = 1000.0*(double)(clock()-te)/CLOCKS_PER_SEC;

    /* cleanup mining structures (BTNs/contracts/tables stay in gu) */
    free(dom);free(issrc);free(srctot);free(keep);free(scoped);
    for(int i=0;i<vcap;i++){ free(cont[i].nx); free(cont[i].cn); }
    free(cont);
    for(int i=0;i<voc.n;i++) free(voc.w[i]);
    free(voc.w);
    return rep->ok ? 0 : -1;
}
