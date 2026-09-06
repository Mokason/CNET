#include "cnet_core_cell.h"
#include "cell_train.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static CnetCoreCell initial,after,snapshot;
static float x[8*3],y[8];
static void init(CnetCoreCell *m){for(int i=0;i<CNET_CELL_WEIGHTS;i++)m->weight[i]=(float)((i*37)%101-50)/100;}
static double sigmoid(double v){return 1/(1+exp(-v));}
static double loss(const CnetCoreCell *m){
    double total=0;
    for(int b=0;b<8;b++){
        double h[8],z=m->weight[40];
        for(int j=0;j<8;j++){double a=m->weight[j*4+3];for(int i=0;i<3;i++)a+=x[b*3+i]*m->weight[j*4+i];h[j]=sigmoid(a);z+=h[j]*m->weight[32+j];}
        double p=sigmoid(z);total-=y[b]*log(p)+(1-y[b])*log1p(-p);
    }
    return total/8;
}
int main(void){
    for(int b=0;b<8;b++){for(int i=0;i<3;i++)x[b*3+i]=(float)((b>>(2-i))&1);y[b]=b!=0;}
    CnetCoreCell zero={{0}};float p=37;
    assert(!cnet_core_cell_predict(&zero,x,&p)&&p==.5f);
    float bad[]={NAN,0,1};assert(cnet_core_cell_predict(&zero,bad,&p)!=0);
    init(&initial);zero.weight[0]=NAN;assert(!cell_gpu_open(&zero,0));
    for(int device=0;device<2;device++){
        for(int sign=-1;sign<=1;sign+=2)for(int magnitude=20;magnitude<=1000;magnitude+=980){
            CnetCoreCell saturated={{0}};saturated.weight[40]=(float)(sign*magnitude);
            CellGpu *s=cell_gpu_open(&saturated,device);assert(s);
            float features[3]={0},label=sign>0?0:1,value=0;
            assert(!cell_gpu_fit(s,features,&label,1,1,.01f,&value));
            assert(fabsf(value-magnitude)<1e-4f);cell_gpu_close(s);
        }
        CellGpu *g=cell_gpu_open(&initial,device);assert(g);assert(!cell_gpu_snapshot(g,&snapshot));
        assert(!memcmp(&snapshot,&initial,sizeof initial));float l=0;
        assert(cell_gpu_fit(g,x,y,0,1,.1f,&l)!=0);
        assert(cell_gpu_fit(g,x,y,2049,1,.1f,&l)!=0);
        assert(cell_gpu_fit(g,x,y,8,0,.1f,&l)!=0);
        assert(!cell_gpu_fit(g,x,y,8,1,.1f,&l));assert(fabs(l-loss(&initial))<1e-5);
        assert(!cell_gpu_snapshot(g,&after));
        double max_error=0;
        for(int i=0;i<CNET_CELL_WEIGHTS;i++){
            CnetCoreCell plus=initial,minus=initial;plus.weight[i]+=.001f;minus.weight[i]-=.001f;
            double numeric=(loss(&plus)-loss(&minus))/(plus.weight[i]-minus.weight[i]);
            double analytic=(initial.weight[i]-after.weight[i])/.1;
            max_error=fmax(max_error,fabs(numeric-analytic));
        }
        assert(max_error<1e-4);assert(!memcmp(&snapshot,&initial,sizeof initial));
        assert(!cell_gpu_fit(g,x,y,8,2000,1,&l));assert(!cell_gpu_snapshot(g,&after));
        float gpu_scores[8];assert(!cell_gpu_predict(g,x,8,gpu_scores));
        assert(cell_gpu_predict(g,x,2049,gpu_scores)!=0);
        for(int b=0;b<8;b++){assert(!cnet_core_cell_predict(&after,x+b*3,&p));assert(fabsf(p-y[b])<.1f);}
        for(int b=0;b<8;b++){assert(!cnet_core_cell_predict(&after,x+b*3,&p));assert(fabsf(p-gpu_scores[b])<1e-6f);}
        assert(!cell_gpu_fail_for_test(g));assert(cell_gpu_snapshot(g,&after)!=0);assert(cell_gpu_fit(g,x,y,8,1,.1f,&l)!=0);cell_gpu_close(g);
        printf("CORE_CELL_GPU_PASS device=%d gradient_max_abs=%.9g finite_fit_rows=8 snapshot_immutable=1 poisoned_refused=1\n",device,max_error);
    }
}
