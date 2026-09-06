#define main investigation_main
#include "investigate.c"
#undef main
int main(void) {
    Task renamed[2]={{.start=0,.goal=7},{.start=1,.goal=7}};
    uint64_t keys[2]={UINT64_C(0x0080000000400004),UINT64_C(0x0080000000400400)};
    float aligned[2][INPUTS];int ordering[8];
    for(int k=0;k<2;k++){
        for(int j=0;j<64;j++)renamed[k].edge[j]=renamed[k].compatible[j]=(keys[k]>>j)&1;
        encode(renamed+k,renamed[k].start,2,aligned[k],NULL,ordering);
    }
    assert(!memcmp(aligned[0],aligned[1],sizeof aligned[0]));
    assert(partition(keys[0])!=partition(keys[1]));
    assert(degree_signature(keys[0])==degree_signature(keys[1]));
    Task blocked={.start=0,.goal=7}; blocked.edge[1]=blocked.compatible[1]=1;
    float scores[9]={0}; scores[7]=.7f;scores[8]=.2f;scores[1]=.1f;
    int identity[8]={0,1,2,3,4,5,6,7}; uint8_t banned[9]={0};
    assert(select_action(&blocked,0,scores,identity,banned,0)==7);
    /* A legal outgoing edge must not force motion on an unreachable task. */
    assert(select_action(&blocked,0,scores,identity,banned,1)==8);
    scores[1]=.8f;assert(select_action(&blocked,0,scores,identity,banned,1)==1);
    banned[1]=1;assert(select_action(&blocked,0,scores,identity,banned,1)==8);
    uint32_t seed=909;
    for (int i=0;i<100;i++) {
        Task t; task_generate(&t,&seed);
        for (int cur=0;cur<8;cur++) if (cur!=t.goal) {
            float features[INPUTS], labels[9], original[9]; int order[8];
            encode(&t,cur,2,features,labels,order);
            Task permuted={0};
            for(int a=0;a<8;a++)for(int b=0;b<8;b++){
                permuted.edge[a*8+b]=t.edge[order[a]*8+order[b]];
                permuted.compatible[a*8+b]=t.compatible[order[a]*8+order[b]];
            }
            assert(degree_signature(effective_graph(&permuted))==degree_signature(effective_graph(&t)));
            assert(order[0]==cur && order[7]==t.goal);
            task_teacher(&t,cur,original);
            for (int a=0;a<8;a++) {
                assert(labels[a]==original[order[a]]);
                for (int b=0;b<8;b++) {
                    if (a!=b) assert(order[a]!=order[b]);
                    assert(features[a*8+b]==(t.edge[order[a]*8+order[b]]&&t.compatible[order[a]*8+order[b]]));
                }
            }
            assert(labels[8]==original[8]);
        }
    }
    puts("CONTROLLER_ENCODING_PASS permutations=700 teacher_labels_equivalent=1 structural_partition_invariant=1 raw_split_counterexample=1 mask_preserves_abstention=1");
    return 0;
}
