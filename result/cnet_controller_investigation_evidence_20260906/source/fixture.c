#include "fixture.h"
#include <string.h>
uint32_t next_random(uint32_t *s) {
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s;
}
void task_generate(Task *t, uint32_t *s) {
    memset(t,0,sizeof *t);
    for (int i=0;i<64;i++) {
        t->edge[i] = i/8!=i%8 && next_random(s)%100<28;
        t->compatible[i] = next_random(s)%100<85;
    }
    t->start=(int)(next_random(s)%8);
    t->goal=(t->start+1+(int)(next_random(s)%7))%8;
}
void task_features(const Task *t, int cur, float x[INPUTS]) {
    memset(x,0,INPUTS*sizeof *x);
    for (int i=0;i<64;i++) { x[i]=t->edge[i]; x[64+i]=t->compatible[i]; }
    x[128+cur]=1; x[136+t->goal]=1;
}
int task_teacher(const Task *t, int cur, float y[ACTIONS]) {
    int d[8], queue[8], head=0, tail=0;
    memset(y,0,ACTIONS*sizeof *y);
    for (int i=0;i<8;i++) d[i]=-1;
    d[t->goal]=0; queue[tail++]=t->goal;
    while (head<tail) {
        int v=queue[head++];
        for (int u=0;u<8;u++)
            if (d[u]<0 && t->edge[u*8+v] && t->compatible[u*8+v]) {
                d[u]=d[v]+1; queue[tail++]=u;
            }
    }
    if (d[cur]<=0) { y[8]=1; return d[cur]; }
    int count=0;
    for (int v=0;v<8;v++)
        if (t->edge[cur*8+v] && t->compatible[cur*8+v] && d[v]==d[cur]-1) {
            y[v]=1; count++;
        }
    for (int v=0;v<8;v++) y[v]/=(float)count;
    return d[cur];
}
int task_check(const Task *t, int cur, int proposal) {
    if (cur<0 || cur>=8 || proposal<0 || proposal>=8) return 0;
    if (!t->edge[8*cur+proposal] || !t->compatible[8*cur+proposal]) return 0;
    return proposal==t->goal ? 2 : 1;
}
uint64_t task_topology(const Task *t) {
    uint64_t h=UINT64_C(14695981039346656037);
    for (int i=0;i<64;i++) { h^=t->edge[i]|(t->compatible[i]<<1); h*=UINT64_C(1099511628211); }
    return h;
}
