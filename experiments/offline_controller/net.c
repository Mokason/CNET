#include "net.h"
#include <math.h>
#include <string.h>
static int linear(cce_amdmath *g,const float *x,const float *w,float *y,int n,int in,int out) {
#ifndef CONTROLLER_CPU_ONLY
    if (g) return cce_amdmath_linear_f32(g,x,w,y,1,n,in,out);
#else
    (void)g;
#endif
    for (int b=0;b<n;b++) for (int o=0;o<out;o++) {
        float v=0; for (int i=0;i<in;i++) v+=x[b*in+i]*w[o*in+i];
        y[b*out+o]=v;
    }
    return 0;
}
static int update(cce_amdmath *g,float *w,const float *x,const float *dy,int n,int in,int out,float lr) {
#ifndef CONTROLLER_CPU_ONLY
    if (g) return cce_amdmath_sgd_f32(g,w,x,dy,1,n,in,out,lr);
#else
    (void)g;
#endif
    for (int o=0;o<out;o++) for (int i=0;i<in;i++) {
        float v=0; for (int b=0;b<n;b++) v+=dy[b*out+o]*x[b*in+i];
        w[o*in+i]-=lr*v;
    }
    return 0;
}
void net_init(Net *m,int depth,int tied,uint32_t seed) {
    memset(m,0,sizeof *m); m->depth=depth; m->tied=tied;
    for (int t=0;t<(tied?1:depth);t++) for (int i=0;i<HIDDEN*JOINED;i++)
        m->w[t][i]=((float)(next_random(&seed)%20001)/10000-1)*.12f;
    for (int i=0;i<ACTIONS*(HIDDEN+1);i++)
        m->out[i]=((float)(next_random(&seed)%20001)/10000-1)*.12f;
}
unsigned long net_parameters(const Net *m) {
    return (unsigned long)(m->tied?1:m->depth)*HIDDEN*JOINED+ACTIONS*(HIDDEN+1);
}
unsigned long net_forward_flops(const Net *m) {
    return 2UL*(m->depth*HIDDEN*JOINED+OUTPUTS*(HIDDEN+1));
}
float net_step(Net *m,Scratch *s,const float *x,const float *y,int n,float lr,cce_amdmath *g) {
    if (!m || !s || !x || n<1 || n>BATCH || m->depth<1 || m->depth>DEPTH || lr<0 || !isfinite(lr)) return -1;
    for (int i=0;i<n*INPUTS;i++) if (!isfinite(x[i])) return -1;
    if (lr>0 && !y) return -1;
    for (int t=0;t<m->depth;t++) {
        float *z=s->z+t*n*JOINED, *h=s->h+t*n*HIDDEN;
        for (int b=0;b<n;b++) {
            memcpy(z+b*JOINED,x+b*INPUTS,INPUTS*sizeof(float));
            if (t) memcpy(z+b*JOINED+INPUTS,s->h+((t-1)*n+b)*HIDDEN,HIDDEN*sizeof(float));
            else memset(z+b*JOINED+INPUTS,0,HIDDEN*sizeof(float));
            z[b*JOINED+JOINED-1]=1;
        }
        if (linear(g,z,m->w[m->tied?0:t],h,n,JOINED,HIDDEN)) return -1;
        for (int i=0;i<n*HIDDEN;i++) h[i]=tanhf(h[i]);
    }
    for (int b=0;b<n;b++) {
        memcpy(s->final+b*(HIDDEN+1),s->h+((m->depth-1)*n+b)*HIDDEN,HIDDEN*sizeof(float));
        s->final[b*(HIDDEN+1)+HIDDEN]=1;
    }
    if (linear(g,s->final,m->out,s->p,n,HIDDEN+1,OUTPUTS)) return -1;
    memset(s->dy,0,n*OUTPUTS*sizeof(float));
    double loss=0;
    for (int b=0;b<n;b++) {
        float *p=s->p+b*OUTPUTS, max=p[0], sum=0;
        for (int a=0;a<ACTIONS;a++) {
            if (!isfinite(p[a])) return -1;
            if (p[a]>max) max=p[a];
        }
        for (int a=0;a<ACTIONS;a++) { p[a]=expf(p[a]-max); sum+=p[a]; }
        for (int a=0;a<ACTIONS;a++) {
            p[a]/=sum;
            if (y) {
                loss-=y[b*ACTIONS+a]*log(fmax(p[a],1e-30f));
                s->dy[b*OUTPUTS+a]=(p[a]-y[b*ACTIONS+a])/n;
            }
        }
    }
    if (!lr) return (float)(loss/n);
    for (int h=0;h<HIDDEN;h++) for (int a=0;a<OUTPUTS;a++)
        s->transpose[h*OUTPUTS+a]=m->out[a*(HIDDEN+1)+h];
    if (linear(g,s->dy,s->transpose,s->dh,n,OUTPUTS,HIDDEN)) return -1;
    for (int t=m->depth-1;t>=0;t--) {
        float *h=s->h+t*n*HIDDEN, *dz=s->dz+t*n*HIDDEN;
        for (int i=0;i<n*HIDDEN;i++) dz[i]=s->dh[i]*(1-h[i]*h[i]);
        if (t) {
            float *w=m->w[m->tied?0:t];
            for (int i=0;i<HIDDEN;i++) for (int o=0;o<HIDDEN;o++)
                s->transpose[i*HIDDEN+o]=w[o*JOINED+INPUTS+i];
            if (linear(g,dz,s->transpose,s->dh,n,HIDDEN,HIDDEN)) return -1;
        }
    }
    /* Only now are all derivatives available, computed using frozen weights. */
    if (update(g,m->out,s->final,s->dy,n,HIDDEN+1,OUTPUTS,lr)) return -1;
    if (m->tied) {
        if (update(g,m->w[0],s->z,s->dz,n*m->depth,JOINED,HIDDEN,lr)) return -1;
    } else for (int t=0;t<m->depth;t++)
        if (update(g,m->w[t],s->z+t*n*JOINED,s->dz+t*n*HIDDEN,n,JOINED,HIDDEN,lr)) return -1;
    return (float)(loss/n);
}
