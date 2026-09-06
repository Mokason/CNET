#include "cnet_core_cell.h"
#include <math.h>
int cnet_core_cell_validate(const CnetCoreCell *m){
    if(!m)return -1;
    for(int i=0;i<CNET_CELL_WEIGHTS;i++)if(!isfinite(m->weight[i]))return -1;
    return 0;
}
static float sigmoid(float v){
    if(v>=0)return 1/(1+expf(-v));
    float e=expf(v);return e/(1+e);
}
int cnet_core_cell_predict(const CnetCoreCell *m,const float x[3],float *p){
    if(!p)return -1;
    *p=0;
    if(!x||cnet_core_cell_validate(m))return -1;
    for(int i=0;i<3;i++)if(!isfinite(x[i])||x[i]<0||x[i]>1)return -1;
    float z=m->weight[40];
    for(int h=0;h<8;h++){
        float a=m->weight[h*4+3];for(int i=0;i<3;i++)a=fmaf(x[i],m->weight[h*4+i],a);
        if(!isfinite(a))return -1;
        z=fmaf(sigmoid(a),m->weight[32+h],z);
    }
    if(!isfinite(z))return -1;
    *p=sigmoid(z);return 0;
}
