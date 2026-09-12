/* Standalone native bounds/sanitizer smoke test for the experiment. */
#include "native.c"
#include <assert.h>
int main(void) {
    Edge edges[]={{0,0,1,1},{1,1,2,1},{0,0,3,1},{3,1,2,1},{2,2,4,-1}};
    void *w=world_create(128,2048,8,19,edges,5);
    int rels[]={0,1};Answer a;
    for(int lane=0;lane<4;++lane)world_query(w,0,rels,2,lane,8,1,&a);
    world_query(w,0,rels,2,0,0,0,&a);assert(a.answer==2);world_free(w);
    for(int lane=0;lane<3;++lane) {
        FactorResult f;
        factor_trial(lane,lane==2?2048:512,128,71,3,7,11,0,.1f,4,&f);
        factor_trial(lane,lane==2?2048:512,128,71,3,7,11,1,.1f,4,&f);
    }
    puts("HDC_SANITIZER_PASS");return 0;
}
