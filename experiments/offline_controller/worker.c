#define _GNU_SOURCE
#include "worker.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
typedef struct {pid_t pid;int fd,mode,device,eof;size_t used;uint64_t started,deadline;char scratch[128];unsigned char bytes[sizeof(WorkerResult)+1];} Job;
struct WorkerPool {char path[4096];Job jobs[2];};
static uint64_t milliseconds(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000;}
/* Reclaim only a job-created root, after its child is reaped. Descriptor-relative
 * traversal never follows symlinks. Unexpected depth/count is retained loudly. */
static int clean_at(int parent,const char *name,unsigned depth,unsigned *budget){
    if(!depth||!*budget)return -1;
    --*budget;int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
    DIR *dir=fdopendir(fd);if(!dir){close(fd);return -1;}int rc=0;struct dirent *e;
    while((e=readdir(dir))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        struct stat s;if(!*budget||fstatat(fd,e->d_name,&s,AT_SYMLINK_NOFOLLOW)){rc=-1;break;}
        if(S_ISDIR(s.st_mode)){if(clean_at(fd,e->d_name,depth-1,budget)){rc=-1;break;}}
        else {--*budget;if(unlinkat(fd,e->d_name,0)){rc=-1;break;}}
    }
    closedir(dir);if(!rc)rc=unlinkat(parent,name,AT_REMOVEDIR);return rc;
}
static void cleanup(const char *scratch){
    if(!scratch[0])return;
    unsigned budget=256;if(clean_at(AT_FDCWD,scratch,5,&budget))fprintf(stderr,"{\"event\":\"worker_scratch_retained\",\"path\":\"%s\"}\n",scratch);
}
WorkerPool *worker_pool_open(const char *path){
    if(!path||path[0]!='/'||strlen(path)>=4096)return NULL;
    struct stat s;if(lstat(path,&s)||!S_ISREG(s.st_mode)||(s.st_uid!=geteuid()&&s.st_uid!=0)||(s.st_mode&022)||access(path,X_OK))return NULL;
    WorkerPool *p=calloc(1,sizeof *p);if(p)strcpy(p->path,path);return p;
}
int worker_cancel(WorkerPool *p,int slot){
    if(!p||slot<0||slot>=2||!p->jobs[slot].pid)return -1;
    Job *j=p->jobs+slot;int status;int rc=kill(j->pid,SIGKILL);if(rc&&errno!=ESRCH)return -1;
    pid_t result;do{result=waitpid(j->pid,&status,0);}while(result<0&&errno==EINTR);
    fprintf(stderr,"{\"event\":\"worker_cancelled\",\"pid\":%ld,\"device\":%d,\"elapsed_ms\":%llu}\n",(long)j->pid,j->device,(unsigned long long)(milliseconds()-j->started));
    close(j->fd);cleanup(j->scratch);memset(j,0,sizeof *j);return result<0?-1:0;
}
void worker_pool_close(WorkerPool *p){if(!p)return;for(int i=0;i<2;i++)if(p->jobs[i].pid)worker_cancel(p,i);free(p);}
static int start(WorkerPool *p,int mode,int device,const CnetCoreCell *snapshot,unsigned timeout){
    if(!p||device<0||device>1||!timeout||timeout>60000||cnet_core_cell_validate(snapshot))return -1;
    int slot=-1;for(int i=0;i<2;i++){if(p->jobs[i].pid&&p->jobs[i].device==device)return -1;if(!p->jobs[i].pid)slot=i;}if(slot<0)return -1;
    int input=memfd_create("cnet-frozen-cell",MFD_CLOEXEC|MFD_ALLOW_SEALING);if(input<0)return -1;
    if(write(input,snapshot,sizeof *snapshot)!=sizeof *snapshot||fcntl(input,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)){close(input);return -1;}
    int descriptors[2];if(pipe2(descriptors,O_CLOEXEC)){close(input);return -1;}
    int readfd=descriptors[0],writefd=fcntl(descriptors[1],F_DUPFD_CLOEXEC,10);close(descriptors[1]);
    int infd=fcntl(input,F_DUPFD_CLOEXEC,10);close(input);int nullfd=open("/dev/null",O_RDWR|O_CLOEXEC);
    int highnull=nullfd>=0?fcntl(nullfd,F_DUPFD_CLOEXEC,10):-1;if(nullfd>=0)close(nullfd);
    char scratch[128]="/tmp/cnet-gpu-worker-XXXXXX";int made=0;
    int rc=-1;if(writefd<0||infd<0||highnull<0)goto done;
    if(!mkdtemp(scratch))goto done;
    made=1;posix_spawn_file_actions_t actions;if(posix_spawn_file_actions_init(&actions))goto done;
    int err=posix_spawn_file_actions_adddup2(&actions,infd,3)||posix_spawn_file_actions_adddup2(&actions,writefd,4)||
        posix_spawn_file_actions_adddup2(&actions,highnull,0)||posix_spawn_file_actions_adddup2(&actions,highnull,1)||
        posix_spawn_file_actions_addclosefrom_np(&actions,5);
    char mode_text[16],device_text[16];snprintf(mode_text,sizeof mode_text,"%d",mode);snprintf(device_text,sizeof device_text,"%d",device);
    char temporary[160];snprintf(temporary,sizeof temporary,"TMPDIR=%s",scratch);
    char *argv[]={p->path,mode_text,device_text,scratch,NULL};char *env[]={"PATH=/usr/bin:/bin","LANG=C",temporary,"AMD_COMGR_CACHE=0",NULL};pid_t pid=0;
    if(!err)err=posix_spawn(&pid,p->path,&actions,NULL,argv,env);
    posix_spawn_file_actions_destroy(&actions);if(err)goto done;
    uint64_t now=milliseconds();p->jobs[slot]=(Job){.pid=pid,.fd=readfd,.mode=mode,.device=device,.started=now,.deadline=now+timeout};
    strcpy(p->jobs[slot].scratch,scratch);made=0;
    fprintf(stderr,"{\"event\":\"worker_started\",\"pid\":%ld,\"device\":%d,\"mode\":%d,\"timeout_ms\":%u}\n",(long)pid,device,mode,timeout);
    if(fcntl(readfd,F_SETFL,O_NONBLOCK)){worker_cancel(p,slot);readfd=-1;goto done;}
    readfd=-1;rc=slot;
done:
    if(made)cleanup(scratch);
    if(readfd>=0)close(readfd);
    if(writefd>=0)close(writefd);
    if(infd>=0)close(infd);
    if(highnull>=0)close(highnull);
    return rc;
}
int worker_start(WorkerPool *p,int mode,int device,const CnetCoreCell *snapshot,unsigned timeout){if(mode!=WORKER_TRAIN&&mode!=WORKER_EVALUATE)return -1;return start(p,mode,device,snapshot,timeout);}
static int read_result(Job *j){
    while(!j->eof&&j->used<sizeof j->bytes){
        ssize_t n=read(j->fd,j->bytes+j->used,sizeof j->bytes-j->used);
        if(n>0)j->used+=(size_t)n;else if(!n)j->eof=1;else if(errno==EINTR)continue;else if(errno==EAGAIN)break;else return -1;
    }
    return 0;
}
int worker_poll(WorkerPool *p,int slot,WorkerResult *out){
    if(out)memset(out,0,sizeof *out);
    if(!p||!out||slot<0||slot>=2||!p->jobs[slot].pid)return -1;
    Job *j=p->jobs+slot;
    if(read_result(j)){worker_cancel(p,slot);return -1;}
    if(j->used>sizeof(WorkerResult)||milliseconds()>=j->deadline){fprintf(stderr,"{\"event\":\"worker_limit_refused\",\"pid\":%ld,\"oversize\":%d}\n",(long)j->pid,j->used>sizeof(WorkerResult));worker_cancel(p,slot);return -1;}
    int status;pid_t finished=waitpid(j->pid,&status,WNOHANG);if(!finished)return 0;
    if(finished<0&&errno==EINTR)return 0;
    /* Exit may race the first nonblocking read: drain again after reaping. */
    int read_failed=read_result(j);
    WorkerResult result={0};int valid=finished==j->pid&&!read_failed&&WIFEXITED(status)&&WEXITSTATUS(status)==0&&j->eof&&j->used==sizeof result;
    if(valid){memcpy(&result,j->bytes,sizeof result);valid=result.magic==UINT32_C(0x43575031)&&result.mode==(unsigned)j->mode&&result.device==(unsigned)j->device&&result.rows==8&&result.sandboxed==1&&
        !cnet_core_cell_validate(&result.cell)&&isfinite(result.loss)&&result.loss>=0&&isfinite(result.max_error)&&result.max_error>=0&&result.max_error<1e-6f;}
    fprintf(stderr,"{\"event\":\"worker_completed\",\"pid\":%ld,\"device\":%d,\"valid\":%d,\"exit\":%d,\"signal\":%d,\"bytes\":%zu,\"elapsed_ms\":%llu}\n",(long)j->pid,j->device,valid,finished>0&&WIFEXITED(status)?WEXITSTATUS(status):-1,finished>0&&WIFSIGNALED(status)?WTERMSIG(status):0,j->used,(unsigned long long)(milliseconds()-j->started));
    close(j->fd);cleanup(j->scratch);memset(j,0,sizeof *j);if(!valid)return -1;*out=result;return 1;
}
#ifdef CONTROLLER_TESTING
int worker_start_fault(WorkerPool *p,int device,int fault,unsigned timeout){CnetCoreCell zero={{0}};if(fault<1||fault>3)return -1;return start(p,100+fault,device,&zero,timeout);}
#endif
