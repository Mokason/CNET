/* Actual allocator decisions for the table representation limitation campaign.
 * This helper supplies no outcome record to any policy. */
#define _GNU_SOURCE
#include "allocator_data.h"
#include "cnet_core_candidate.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc,char **argv){
    if(argc!=4)return 2;
    int dir=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dir<0)return 2;
    CnetCoreCandidate candidate;
    int bad=cnet_core_candidate_load_objective_at(dir,argv[2],&candidate,CNET_CORE_OBJECTIVE_ALLOCATOR);
    close(dir);if(bad)return 2;
    AllocatorData *data=calloc(1,sizeof *data);if(!data)return 2;
    if(allocator_data_read(argv[3],0,data)||data->n!=1){free(data);return 2;}
    CnetAllocatorEpisode *e=data->episode;
    printf("{\"input_sha256\":\"%s\",\"choices\":[",data->sha256);
    for(unsigned policy=0;policy<4;policy++){
        CnetAllocatorChoice choice;
        if(cnet_core_allocator_choose(&candidate.cell,e->task,e->n,e->cursor,e->jobs,e->work,e->wait_limit,policy,&choice)){free(data);return 2;}
        printf("%s{\"policy\":%u,\"work\":%u,\"tasks\":[",policy?",":"",policy,choice.work);
        for(unsigned i=0;i<choice.count;i++)printf("%s%llu",i?",":"",(unsigned long long)e->task[choice.index[i]].id);
        printf("]}");
    }
    puts("]}");free(data);return 0;
}
