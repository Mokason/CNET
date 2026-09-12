/* Experimental sparse-only routing. Inputs are validated once by the loader. */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
extern int vr_terms(const char *, uint64_t *, int);
typedef struct {
    const uint64_t *keys;
    const uint32_t *offset;
    const int32_t *owners;
    const float *weights;
    int nkeys, nc;
} FastSparse;
static int find_key(const uint64_t *keys, int n, uint64_t key) {
    int lo=0, hi=n;
    while(lo<hi) { int mid=lo+(hi-lo)/2; if(keys[mid]<key)lo=mid+1;else hi=mid; }
    return lo<n && keys[lo]==key ? lo : -1;
}
static void sparse_score(const FastSparse *s,const uint64_t *terms,int nt,float *out) {
    memset(out,0,(size_t)s->nc*sizeof(float));
    for(int i=0;i<nt;++i) {
        int duplicate=0;
        for(int j=0;j<i;++j)if(terms[j]==terms[i]){duplicate=1;break;}
        if(duplicate)continue;
        int t=find_key(s->keys,s->nkeys,terms[i]);
        if(t<0)continue;
        for(uint32_t j=s->offset[t];j<s->offset[t+1];++j)out[s->owners[j]]+=s->weights[j];
    }
}
int fast_top(const float *scores,int nc,int k,int32_t *out) {
    int n=0;
    for(int c=0;c<nc;++c) {
        if(n==k && scores[c]<=scores[out[n-1]])continue;
        int j=n<k?n++:k-1;
        while(j>0 && scores[c]>scores[out[j-1]]){out[j]=out[j-1];--j;}
        out[j]=c;
    }
    return n;
}
int fast_scores(const char *text,const FastSparse *a,const FastSparse *b,int mode,float *out) {
    uint64_t terms[512];
    int nt=vr_terms(text,terms,512);
    if(nt<0)return -1;
    int nc=a->nc;
    if(nc<1 || nc>4096 || b->nc!=nc)return -2;
    float sa[nc],sb[nc];
    sparse_score(a,terms,nt,sa);sparse_score(b,terms,nt,sb);
    if(mode==0){memcpy(out,sb,(size_t)nc*sizeof(float));return nt;}
    memset(out,0,(size_t)nc*sizeof(float));
    int32_t top[20];
    for(int branch=0;branch<2;++branch) {
        float *s=branch?sb:sa;
        int nk=fast_top(s,nc,nc<20?nc:20,top);
        for(int r=0;r<nk;++r)if(s[top[r]]>0)out[top[r]]+=1.0/(61+r);
    }
    return nt;
}
int fast_query(const char *text,const FastSparse *a,const FastSparse *b,int mode,int k,int32_t *ids,float *values) {
    if(a->nc<1 || a->nc>4096 || k<1 || k>20)return -1;
    float scores[a->nc];
    if(fast_scores(text,a,b,mode,scores)<0)return -2;
    int n=fast_top(scores,a->nc,k<a->nc?k:a->nc,ids),positive=0;
    for(int i=0;i<n;++i){values[i]=scores[ids[i]];if(values[i]>0)++positive;}
    return positive;
}
