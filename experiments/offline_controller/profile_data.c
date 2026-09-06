#define main investigation_main
#include "investigate.c"
#undef main
/* Replay the accepted streaming fixture sequence, never the model. */
int main(void) {
    read_tasks("validation.tsv",validation_tasks,512);
    uint64_t held[512];for(int i=0;i<512;i++)held[i]=degree_signature(effective_graph(validation_tasks+i));
    qsort(held,512,sizeof *held,key_compare);int unique=1;for(int i=1;i<512;i++)unique+=held[i]!=held[i-1];
    for(unsigned seed=1;seed<=3;seed++) {
        uint32_t stream=seed*87101;unsigned long generated=0,rejected_partition=0,rejected_validation=0,distances[8]={0};
        for(int i=0;i<30000*BATCH;i++) {
            Task t;uint64_t key;
            for(;;){
                task_generate(&t,&stream);generated++;key=degree_signature(effective_graph(&t));
                if(key%5==0){rejected_partition++;continue;}
                if(bsearch(&key,held,512,sizeof key,key_compare)){rejected_validation++;continue;}
                break;
            }
            float y[9];int d=task_teacher(&t,t.start,y);distances[d<0?0:d]++;
        }
        printf("{\"kind\":\"stream_data_profile\",\"seed\":%u,\"accepted\":%d,\"generated\":%lu,\"rejected_partition\":%lu,\"rejected_validation\":%lu,\"validation_signatures\":%d,\"unreachable\":%lu,\"distance_cases\":[",seed,30000*BATCH,generated,rejected_partition,rejected_validation,unique,distances[0]);
        for(int d=1;d<8;d++)printf("%s%lu",d==1?"":",",distances[d]);
        puts("]}");
    }
    return 0;
}
