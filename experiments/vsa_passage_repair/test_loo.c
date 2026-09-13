#include "cnet_vsa_gen_capsule.h"
#include "cnet_vsa_lexicon.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
extern int cnet_vsa_text_leave_one_out_distances(const CnetVsaTokenList *,uint32_t,const int8_t *,float,float,float *,size_t);
int main(void) {
    CnetVsaLexicon lex;assert(!cnet_vsa_lexicon_load(&lex,"bin/registry.lex"));cnet_vsa_lexicon_set_active(&lex);
    int8_t target[CNET_VSA_TOPICAL_DIM];assert(!cnet_vsa_gencap_encode_intent_q8("temperature pressure heat transfer fluid wavefront shell",target,CNET_VSA_ENCODER_LEX));
    float tn=cnet_vsa_text_q8_norm(target);char line[4096];int queries=0,checks=0;
    while(fgets(line,sizeof(line),stdin)) {
        CnetVsaTokenList tl;if(cnet_vsa_text_tokenize(line,&tl)<=0)continue;
        int content=0;for(size_t i=0;i<tl.count;++i)content+=!cnet_vsa_text_is_stopword(tl.tokens[i].token);
        if(content<2 || tl.count>125)continue;
        for(int mode=0;mode<2;++mode) {
            float radius=mode?.8f:2.f,dist[CNET_VSA_MAX_TOKENS];
            int got=cnet_vsa_text_leave_one_out_distances(&tl,CNET_VSA_ENCODER_LEX,target,tn,radius,dist,CNET_VSA_MAX_TOKENS);
            assert(got>0);int k=0;
            for(size_t removed=0;removed<tl.count;++removed) {
                if(cnet_vsa_text_is_stopword(tl.tokens[removed].token))continue;
                char text[CNET_VSA_MAX_TOKENS*(CNET_VSA_TOKEN_LEN+1)+1];size_t pos=0;
                for(size_t i=0;i<tl.count;++i)if(i!=removed){size_t len=strlen(tl.tokens[i].token);memcpy(text+pos,tl.tokens[i].token,len);pos+=len;text[pos++]=' ';}text[pos]=0;
                int8_t v[CNET_VSA_TOPICAL_DIM];assert(!cnet_vsa_gencap_encode_intent_q8(text,v,CNET_VSA_ENCODER_LEX));
                float expected=1-cnet_vsa_text_q8_similarity_n(v,cnet_vsa_text_q8_norm(v),target,tn);
                assert(k<got);assert(dist[k++]==expected);checks++;
                if(expected>radius)break;
            }
            assert(k==got);
        }
        queries++;
    }
    cnet_vsa_lexicon_free(&lex);printf("CNET_VSA_LOO_PARITY_PASS queries=%d distances=%d\n",queries,checks);return queries?0:1;
}
