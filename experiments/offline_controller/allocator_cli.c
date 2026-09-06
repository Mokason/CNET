#define _GNU_SOURCE
#include "allocator_data.h"
#include "worker.h"
#include "cnet_core_candidate.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static int digest(const char *s){
    if(!s||strlen(s)!=64)return -1;
    int nonzero=0;for(unsigned i=0;i<64;i++){if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return -1;nonzero|=s[i]!='0';}
    return nonzero?0:-1;
}
static int train(int argc,char **argv){
    if(argc!=8&&argc!=9)return 2;
    if((strcmp(argv[3],"0")&&strcmp(argv[3],"1"))||digest(argv[7]))return 2;
    AllocatorData *data=calloc(1,sizeof *data);WorkerBatch *batch=calloc(1,sizeof *batch);
    int dir=-1,rc=2;WorkerPool *pool=NULL;
    if(!data||!batch||allocator_data_read(argv[4],1,data))goto done;
    dir=open(argv[5],O_RDONLY|O_NOFOLLOW|O_DIRECTORY|O_CLOEXEC);if(dir<0)goto done;
    CnetCoreCandidate candidate={0};
    if(argc==9){if(cnet_core_candidate_load_objective_at(dir,argv[8],&candidate,CNET_CORE_OBJECTIVE_ALLOCATOR))goto done;}
    else for(unsigned i=0;i<41;i++)candidate.cell.weight[i]=(float)((int)(i*37%101)-50)/100;
    batch->magic=WORKER_BATCH_MAGIC;batch->feature_version=1;batch->epochs=4096;batch->lr=1;batch->cell=candidate.cell;
    for(size_t i=0;i<data->n;i++)for(unsigned j=0;j<data->episode[i].n;j++){
        unsigned row=batch->rows++;memcpy(batch->x+3*row,data->episode[i].task[j].features,3*sizeof(float));
        batch->y[row]=(float)__builtin_popcountll(data->episode[i].coverage[j])/data->episode[i].population;
    }
    pool=worker_pool_open(argv[2]);if(!pool)goto done;
    int slot=worker_start_batch(pool,WORKER_BATCH_TRAIN,argv[3][0]-'0',batch,30000);if(slot<0)goto done;
    WorkerResult result;int status;
    do{status=worker_poll(pool,slot,&result);if(!status){struct timespec pause={0,1000000};nanosleep(&pause,NULL);}}while(!status);
    if(status!=1)goto done;
    candidate.cell=result.cell;memcpy(candidate.training_sha256,data->sha256,65);memcpy(candidate.evaluation_sha256,argv[7],65);
    if(cnet_core_candidate_save_objective_at(dir,argv[6],&candidate,CNET_CORE_OBJECTIVE_ALLOCATOR))goto done;
    printf("{\"event\":\"allocator_trained\",\"device\":%s,\"rows\":%u,\"epochs\":4096,\"training_sha256\":\"%s\",\"evaluation_sha256\":\"%s\",\"parity_max_abs\":%.9g,\"candidate\":\"%s\",\"approved\":false}\n",
        argv[3],batch->rows,data->sha256,argv[7],result.max_error,argv[6]);rc=0;
done:worker_pool_close(pool);if(dir>=0)close(dir);free(data);free(batch);return rc;
}
static int inspect(int argc,char **argv,int evaluate){
    if(evaluate?(argc!=6&&argc!=7):argc!=5)return 2;
    int dir=open(argv[2],O_RDONLY|O_NOFOLLOW|O_DIRECTORY|O_CLOEXEC);if(dir<0)return 2;
    CnetCoreCandidate candidate,active;int bad=cnet_core_candidate_load_objective_at(dir,argv[3],&candidate,CNET_CORE_OBJECTIVE_ALLOCATOR);
    if(!bad&&evaluate&&argc==7)bad=cnet_core_candidate_load_objective_at(dir,argv[6],&active,CNET_CORE_OBJECTIVE_ALLOCATOR);
    close(dir);if(bad)return 2;
    AllocatorData *data=calloc(1,sizeof *data),*training=evaluate?calloc(1,sizeof *training):NULL;int rc=2;
    if(!data||(evaluate&&!training)||allocator_data_read(argv[evaluate?5:4],evaluate,data))goto done;
    if(evaluate){
        if(allocator_data_read(argv[4],1,training)||strcmp(training->sha256,candidate.training_sha256)||
           strcmp(data->sha256,candidate.evaluation_sha256)||allocator_data_disjoint(training,data))goto done;
        CnetAllocatorGateReport r;int status=cnet_core_allocator_evaluate_against(&candidate.cell,argc==7?&active.cell:NULL,data->episode,data->n,&r);if(status<0)goto done;
        printf("{\"event\":\"allocator_evaluation\",\"passed\":%s,\"episodes\":%zu,\"comparators\":%u,\"training_sha256\":\"%s\",\"evaluation_sha256\":\"%s\",\"active_training_sha256\":\"%s\",\"coverage\":[",
            r.passed?"true":"false",r.episodes,r.comparators,training->sha256,data->sha256,argc==7?active.training_sha256:"");
        for(unsigned j=0;j<=r.comparators;j++)printf("%s%.9g",j?",":"",r.coverage[j]);
        printf("],\"gain\":[");for(unsigned j=0;j<r.comparators;j++)printf("%s%.9g",j?",":"",r.gain[j]);
        printf("],\"paired95_lower\":[");for(unsigned j=0;j<r.comparators;j++)printf("%s%.9g",j?",":"",r.paired95_lower[j]);
        printf("],\"family_gain\":[");
        for(unsigned f=0;f<4;f++){printf("%s[",f?",":"");for(unsigned j=0;j<r.comparators;j++)printf("%s%.9g",j?",":"",r.family_gain[f][j]);printf("]");}
        printf("],\"minimum_gain\":0.05,\"paired_t_multiplier\":2.040,\"autoactivation\":false}\n");rc=status?3:0;
    }else{
        if(data->n!=1)goto done;
        CnetAllocatorEpisode *e=data->episode;CnetAllocatorChoice p;
        if(cnet_core_allocator_choose(&candidate.cell,e->task,e->n,e->cursor,e->jobs,e->work,e->wait_limit,0,&p))goto done;
        printf("{\"event\":\"allocator_choice\",\"input_sha256\":\"%s\",\"episode\":%llu,\"work\":%u,\"tasks\":[",data->sha256,(unsigned long long)e->id,p.work);
        for(unsigned i=0;i<p.count;i++)printf("%s%llu",i?",":"",(unsigned long long)e->task[p.index[i]].id);
        puts("]}");rc=0;
    }
done:free(data);free(training);return rc;
}
int main(int argc,char **argv){
    int rc=2;
    if(argc>1&&!strcmp(argv[1],"train"))rc=train(argc,argv);
    else if(argc>1&&!strcmp(argv[1],"evaluate"))rc=inspect(argc,argv,1);
    else if(argc>1&&!strcmp(argv[1],"choose"))rc=inspect(argc,argv,0);
    if(rc==2)fputs("CORE_ALLOCATOR_REFUSED invalid_arguments_data_checkpoint_or_worker\n",stderr);
    return rc;
}
