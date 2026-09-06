#include "cnet_core_selector.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void){
    /* A test-only explicit cell isolates graph mechanics from training. This
     * artifact is never admitted or counted as a GPU-fit result. */
    CnetCoreCell cell={{0}};for(int i=0;i<3;i++)cell.weight[i]=20;
    cell.weight[3]=-10;cell.weight[32]=20;cell.weight[40]=-10;
    CnetSelectorGraph g={0};g.feature_version=1;g.generation=1;g.n=64;g.start=0;g.goal=63;
    for(unsigned i=0;i<64;i++){
        g.node[i]=(CnetSelectorNode){i+1,1,1,1,1};if(i<63)g.edge[i]=UINT64_C(1)<<(i+1);
    }
    CnetSelectorProposal p;
    assert(!cnet_core_selector_propose(&cell,&g,64,&p));assert(p.count==1&&p.ranked[0]==1&&p.distance==63);
    assert(p.hops==63);for(unsigned i=0;i<p.hops;i++)assert(p.path[i]==i+1);
    assert(!cnet_core_selector_propose(&cell,&g,1,&p)&&p.count==0);
    g.node[1].input_type=2;assert(!cnet_core_selector_propose(&cell,&g,64,&p)&&p.count==0);g.node[1].input_type=1;
    g.node[1].available=0;assert(!cnet_core_selector_propose(&cell,&g,64,&p)&&p.count==0);g.node[1].available=1;
    g.node[1].identity=1;assert(cnet_core_selector_propose(&cell,&g,64,&p)!=0&&p.count==0);g.node[1].identity=2;
    g.n=65;assert(cnet_core_selector_propose(&cell,&g,64,&p)!=0);g.n=64;
    g.feature_version=2;assert(cnet_core_selector_propose(&cell,&g,64,&p)!=0);g.feature_version=1;
    CnetSelectorGraph r=g;
    for(unsigned i=0;i<64;i++){r.node[63-i]=g.node[i];r.edge[63-i]=i<63?UINT64_C(1)<<(62-i):0;}
    r.start=63;r.goal=0;assert(!cnet_core_selector_propose(&cell,&r,64,&p));assert(p.count==1&&p.ranked[0]==62&&p.distance==63);
    cell.weight[0]=NAN;assert(cnet_core_selector_propose(&cell,&g,64,&p)!=0&&p.count==0);
    puts("CORE_SELECTOR_MECHANICS_PASS long_chain=63 relabel=1 incompatible_abstain=1 unavailable_abstain=1 malformed_refused=4");
}
