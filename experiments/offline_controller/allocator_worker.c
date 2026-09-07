#define _GNU_SOURCE
#include "worker.h"
#include "cell_train.h"
#include "worker_sandbox.h"
#include "offline.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
static WorkerBatch batch;
static float scores[2048];
static int number(const char *s,int max){
    char *end;errno=0;long v=strtol(s,&end,10);
    return !s[0]||errno||*end||v<0||v>max?-1:(int)v;
}
int main(int argc,char **argv){
    if(argc!=5)return 2;
    if(worker_guard_enter(argv[4]))return 3;
    int mode=number(argv[1],4),device=number(argv[2],1);
    if(device<0||(mode!=WORKER_BATCH_TRAIN&&mode!=WORKER_BATCH_EVALUATE))return 2;
    if(close_range(5,~0u,0))return 3;
    struct rlimit limit={0,0};if(setrlimit(RLIMIT_CORE,&limit))return 3;
    limit=(struct rlimit){30,30};if(setrlimit(RLIMIT_CPU,&limit))return 3;
    limit=(struct rlimit){1024,1024};if(setrlimit(RLIMIT_NOFILE,&limit))return 3;
    limit=(struct rlimit){16*1024*1024,16*1024*1024};if(setrlimit(RLIMIT_FSIZE,&limit))return 3;
    /* No bytes from FD3 have been read. Fixed trusted warm-up creates the HIP
     * runtime and all fit/predict kernels before process/network confinement. */
    if(worker_filesystem_seal(argv[3]))return 4;
    CnetCoreCell warm={{0}};float x[3]={0},y=0,score,loss;
    CellGpu *gpu=cell_gpu_open(&warm,device);if(!gpu)return 5;
    if(cell_gpu_predict(gpu,x,1,&score)||cell_gpu_fit(gpu,x,&y,1,1,.01f,&loss)||
       offline_seal(-1)||worker_process_seal()||offline_negative_test())return 6;
    /* The supplied snapshot and labels cross the boundary only now. */
    int seals=F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL;
    struct stat input,output;
    if(fstat(3,&input)||!S_ISREG(input.st_mode)||input.st_size!=sizeof batch||fcntl(3,F_GET_SEALS)!=seals||
       fstat(4,&output)||!S_ISFIFO(output.st_mode)||pread(3,&batch,sizeof batch,0)!=sizeof batch||worker_batch_validate(&batch))return 7;
    close(3);
    if(cell_gpu_restore(gpu,&batch.cell))return 8;
    WorkerResult r={.magic=UINT32_C(0x43575031),.mode=(unsigned)mode,.device=(unsigned)device,.rows=batch.rows,.sandboxed=1};
    if(mode==WORKER_BATCH_TRAIN&&cell_gpu_fit(gpu,batch.x,batch.y,(int)batch.rows,(int)batch.epochs,batch.lr,&r.loss))return 8;
    if(cell_gpu_snapshot(gpu,&r.cell)||cell_gpu_predict(gpu,batch.x,(int)batch.rows,scores))return 8;
    if(mode==WORKER_BATCH_EVALUATE&&memcmp(&r.cell,&batch.cell,sizeof r.cell))return 8;
    double squared=0;
    for(unsigned i=0;i<batch.rows;i++){
        float cpu;if(cnet_core_cell_predict(&r.cell,batch.x+3*i,&cpu))return 8;
        r.max_error=fmaxf(r.max_error,fabsf(cpu-scores[i]));
        double error=(double)cpu-batch.y[i];squared+=error*error;
    }
    if(mode==WORKER_BATCH_EVALUATE)r.loss=(float)(squared/batch.rows);
    if(r.max_error>=1e-6f)return 8;
    cell_gpu_close(gpu);
    size_t used=0;while(used<sizeof r){ssize_t n=write(4,(char*)&r+used,sizeof r-used);if(n<0&&errno==EINTR)continue;if(n<=0)return 9;used+=(size_t)n;}
    close(4);return 0;
}
