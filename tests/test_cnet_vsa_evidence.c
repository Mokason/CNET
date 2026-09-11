#include "cnet_vsa_evidence.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    CnetVsaEvidenceFact facts[]={{0,0,1,1},{1,1,2,1},{0,0,3,1},{3,1,2,1},{99,2,98,-1}};
    int rels[]={0,1};char out[512];
    assert(cnet_vsa_evidence_response(facts,5,0,rels,2,2,out,sizeof out)==0);
    assert(!strcmp(out,"node002. node000 rel00 node001. node001 rel01 node002."));
    assert(cnet_vsa_evidence_response(facts,5,0,rels,2,3,out,sizeof out)==1);
    assert(!strncmp(out,"ABSTAIN:",8));
    assert(cnet_vsa_evidence_response(facts,5,0,rels,1,-1,out,sizeof out)==1);
    assert(cnet_vsa_evidence_response(facts,5,2,rels,2,-1,out,sizeof out)==1);
    facts[4]=(CnetVsaEvidenceFact){3,1,2,-1};
    assert(cnet_vsa_evidence_response(facts,5,0,rels,2,2,out,sizeof out)==1);
    facts[4]=(CnetVsaEvidenceFact){99,2,98,-1};
    assert(cnet_vsa_evidence_response(facts,5,0,rels,2,2,out,8)==-2 && !out[0]);
    assert(cnet_vsa_evidence_response(facts,5,0,rels,0,2,out,sizeof out)==-1 && !out[0]);
    assert(cnet_vsa_evidence_response(facts,5,0,rels,9,2,out,sizeof out)==-1 && !out[0]);
    assert(cnet_vsa_evidence_response(facts,513,0,rels,2,2,out,sizeof out)==-1);
    assert(cnet_vsa_evidence_response(NULL,0,0,rels,2,-1,out,sizeof out)==1);
    assert(cnet_vsa_evidence_response(NULL,1,0,rels,2,-1,out,sizeof out)==-1);
    assert(cnet_vsa_evidence_response(facts,5,128,rels,2,-1,out,sizeof out)==-1);
    assert(cnet_vsa_evidence_response(facts,5,0,rels,2,-2,out,sizeof out)==-1);
    assert(cnet_vsa_evidence_response(facts,5,0,rels,2,2,NULL,512)==-1);
    facts[4].sign=0;
    assert(cnet_vsa_evidence_response(facts,5,0,rels,2,2,out,sizeof out)==-1);
    facts[4].sign=1;facts[4].object=128;
    assert(cnet_vsa_evidence_response(facts,5,0,rels,2,2,out,sizeof out)==-1);
    CnetVsaEvidenceFact chain[8];int steps[8];
    for(int i=0;i<8;++i){chain[i]=(CnetVsaEvidenceFact){i,i,i+1,1};steps[i]=i;}
    assert(cnet_vsa_evidence_response(chain,8,0,steps,8,8,out,sizeof out)==0);
    assert(strstr(out,"node007 rel07 node008."));
    puts("CNET_VSA_EVIDENCE_PASS");return 0;
}
