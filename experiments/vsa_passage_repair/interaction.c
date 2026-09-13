/* Offline candidate: fixed 64-dimensional word similarities and 11 RBF bins. */
#include <math.h>
#include <stddef.h>
int ip_pool(const float *q,const float *weights,int nq,const float *d,int nd,float *out) {
    if(!q||!weights||!d||!out||nq<=0||nd<=0||nq>512||nd>512)return -1;
    for(int k=0;k<25;k++)out[k]=0;
    out[23]=1;float total=0;
    for(int i=0;i<nq;i++) {
        if(!isfinite(weights[i])||weights[i]<0)return -1;
        float mass[11]={0},best=-1;
        for(int j=0;j<nd;j++) {
            float s=0;for(int z=0;z<64;z++)s+=q[i*64+z]*d[j*64+z];
            if(!isfinite(s))return -1;
            s=fmaxf(-1,fminf(1,s));best=fmaxf(best,s);
            for(int k=0;k<11;k++) {
                float mu=-1.f+.2f*k,sigma=k==10?.001f:.1f,x=(s-mu)/sigma;
                mass[k]+=expf(-.5f*x*x);
            }
        }
        for(int k=0;k<11;k++){out[k]+=weights[i]*log1pf(mass[k]);out[k+11]+=weights[i]*mass[k]/nd;}
        out[22]+=weights[i]*best;out[23]=fminf(out[23],best);out[24]+=weights[i]*(best>=.5f);total+=weights[i];
    }
    if(!(total>0))return -1;
    for(int k=0;k<25;k++)if(k!=23)out[k]/=total;
    return 0;
}
