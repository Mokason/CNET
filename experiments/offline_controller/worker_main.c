#define _GNU_SOURCE
#include "worker.h"
#include "cell_train.h"
#include "worker_sandbox.h"
#include "offline.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
static int number(const char *s,int max){char *end;errno=0;long v=strtol(s,&end,10);return !s[0]||errno||*end||v<0||v>max?-1:(int)v;}
int main(int argc,char **argv){
    if(argc!=4)return 2;
    int mode=number(argv[1],103),device=number(argv[2],1);
    if(device<0||(mode!=WORKER_TRAIN&&mode!=WORKER_EVALUATE
#ifdef CONTROLLER_TESTING
        &&(mode<101||mode>103)
#endif
    ))return 2;
    int seals=F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL;
    struct stat input,output;CnetCoreCell cell;
    if(fstat(3,&input)||input.st_size!=sizeof cell||fcntl(3,F_GET_SEALS)!=seals||fstat(4,&output)||!S_ISFIFO(output.st_mode)||
        pread(3,&cell,sizeof cell,0)!=sizeof cell||cnet_core_cell_validate(&cell))return 3;
    close(3);if(close_range(5,~0u,0))return 3;
    struct rlimit limit={0,0};if(setrlimit(RLIMIT_CORE,&limit))return 3;
    limit=(struct rlimit){30,30};if(setrlimit(RLIMIT_CPU,&limit))return 3;
    limit=(struct rlimit){1024,1024};if(setrlimit(RLIMIT_NOFILE,&limit))return 3;
    limit=(struct rlimit){16*1024*1024,16*1024*1024};if(setrlimit(RLIMIT_FSIZE,&limit))return 3;
    /* HIP reserves a large virtual address aperture; RLIMIT_AS would measure
     * that aperture, not resident memory. Tensor/epoch bounds enforce workload
     * limits; driver/runtime allocation is measured separately, not hard-capped. */
    if(worker_filesystem_seal(argv[3]))return 4;
    CellGpu *gpu=cell_gpu_open(&cell,device);if(!gpu)return 5;
    float x[24],y[8],scores[8];for(unsigned b=0;b<8;b++){y[b]=b!=0;for(unsigned i=0;i<3;i++)x[b*3+i]=(float)((b>>(2-i))&1);}
    if(cell_gpu_predict(gpu,x,8,scores)||offline_seal(-1)||worker_process_seal()||offline_negative_test())return 6;
    WorkerResult result={.magic=UINT32_C(0x43575031),.mode=(unsigned)mode,.device=(unsigned)device,.rows=8,.sandboxed=1};
#ifdef CONTROLLER_TESTING
    if(mode==101)return 9;
    if(mode==102){if(write(4,&result,sizeof result/2)<0)return 9;return 0;}
    if(mode==103){for(;;)pause();}
#endif
    if(mode==WORKER_TRAIN&&cell_gpu_fit(gpu,x,y,8,2000,1,&result.loss))return 7;
    if(cell_gpu_snapshot(gpu,&result.cell)||cell_gpu_predict(gpu,x,8,scores))return 7;
    for(unsigned b=0;b<8;b++){
        float score;if(cnet_core_cell_predict(&result.cell,x+b*3,&score)||fabsf(score-y[b])>.1f)return 8;
        result.max_error=fmaxf(result.max_error,fabsf(score-scores[b]));
    }
    if(result.max_error>=1e-6f)return 8;
    cell_gpu_close(gpu);size_t used=0;while(used<sizeof result){ssize_t n=write(4,(char*)&result+used,sizeof result-used);if(n<0&&errno==EINTR)continue;if(n<=0)return 10;used+=(size_t)n;}
    close(4);return 0;
}
