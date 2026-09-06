/* Instantiate the frozen experiment's data/feature helpers without changing
 * its source or data assignment. This is a trainer, never a certifier. */
#define main investigation_main
#include "investigate.c"
#undef main
#include "resident.h"
static float batches[8][BATCH*INPUTS],labels[8][BATCH*ACTIONS];
int main(int argc,char **argv){
    assert(argc==5);char *end;
    unsigned seed=(unsigned)strtoul(argv[1],&end,10);assert(!*end&&seed>=1&&seed<=3);
    long device=strtol(argv[2],&end,10);assert(!*end&&device>=0&&device<2);
    long steps=strtol(argv[3],&end,10);assert(!*end&&steps>=8&&steps<=30000&&steps%8==0);
    assert(!close_range(3,~0u,0));read_tasks(argv[4],validation_tasks,512);
    uint64_t held[512];for(int i=0;i<512;i++)held[i]=degree_signature(effective_graph(validation_tasks+i));
    qsort(held,512,sizeof *held,key_compare);
    net_init(&candidate,4,1,seed*997);Resident *r=resident_open(&candidate,(int)device);assert(r);
    printf("{\"kind\":\"resident_backend\",\"name\":\"%s\",\"device\":%ld}\n",resident_backend(r),device);
    assert(!offline_seal(-1)&&!offline_negative_test());
    uint32_t stream=seed*87101;unsigned long raw=0,ood=0;float losses[8];double start=seconds(),updates=0;
    for(int step=0;step<steps;step+=8){
        for(int c=0;c<8;c++)for(int b=0;b<BATCH;b++){
            Task t;uint64_t key;int order[8];
            do{task_generate(&t,&stream);raw++;key=degree_signature(effective_graph(&t));}
            while(key%5==0||bsearch(&key,held,512,sizeof key,key_compare));
            encode(&t,t.start,2,batches[c]+b*INPUTS,labels[c]+b*ACTIONS,order);ood+=labels[c][b*ACTIONS+8]>0;
        }
        double before=seconds();assert(!resident_steps(r,&batches[0][0],&labels[0][0],8,BATCH,.15f,losses));updates+=seconds()-before;
    }
    assert(!resident_snapshot(r,&candidate));resident_close(r);
    char file[80];snprintf(file,sizeof file,"resident-%u-%ld.weights",seed,steps);
    FILE *f=fopen(file,"wx");assert(f);assert(fwrite(&candidate,sizeof candidate,1,f)==1);assert(!fclose(f));
    printf("{\"kind\":\"resident_training\",\"seed\":%u,\"device\":%ld,\"steps\":%ld,\"batch\":128,\"accepted_examples\":%ld,\"raw_draws\":%lu,\"unreachable_examples\":%lu,\"update_seconds\":%.6f,\"wall_seconds\":%.6f,\"last_loss\":%.9g,\"offline_sealed\":true,\"labels\":\"independent_BFS\"}\n",seed,device,steps,steps*BATCH,raw,ood,updates,seconds()-start,losses[7]);
}
