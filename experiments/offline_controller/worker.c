#define _GNU_SOURCE
#include "worker.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/pidfd.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
typedef struct {pid_t pid;int fd,mode,device,eof;unsigned rows;size_t used;uint64_t started,deadline;char scratch[128];unsigned char bytes[sizeof(WorkerResult)+1];} Job;
struct WorkerPool {char path[4096];Job jobs[2];};
#ifndef PIDFD_THREAD
/* Linux 6.9 UAPI; older kernels reject the flag, without a weaker fallback. */
#define PIDFD_THREAD O_EXCL
#endif
static uint64_t milliseconds(void){struct timespec t;if(clock_gettime(CLOCK_BOOTTIME,&t))return 0;return (uint64_t)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000;}
static uint64_t elapsed(uint64_t started){uint64_t now=milliseconds();return now>=started?now-started:0;}
int worker_guard_enter(const char *deadline_text){
    /* The pool captures the exact spawning thread before spawn. Arm first,
     * then inspect its pidfd: an already-dead thread must not escape the
     * PR_SET_PDEATHSIG pre-arm race, even while its process stays alive. */
    if(prctl(PR_SET_PDEATHSIG,SIGKILL,0,0,0))return -1;
    char target[64];ssize_t n=readlink("/proc/self/fd/5",target,sizeof target-1);
    if(n<0)return -1;
    target[n]=0;if(strcmp(target,"anon_inode:[pidfd]"))return -1;
    struct pollfd owner={.fd=5,.events=POLLIN};
    if(poll(&owner,1,0)!=0||close_range(5,~0u,0))return -1;

    if(!deadline_text||deadline_text[0]<'0'||deadline_text[0]>'9')return -1;
    char *end;errno=0;unsigned long long deadline=strtoull(deadline_text,&end,10);
    uint64_t now=milliseconds();
    if(errno||*end||!now||deadline<=now||deadline-now>60000)return -1;
    struct sigevent event={.sigev_notify=SIGEV_SIGNAL,.sigev_signo=SIGKILL};timer_t timer;
    struct itimerspec expires={.it_value={.tv_sec=(time_t)(deadline/1000),.tv_nsec=(long)(deadline%1000)*1000000}};
    if(timer_create(CLOCK_BOOTTIME,&event,&timer))return -1;
    if(timer_settime(timer,TIMER_ABSTIME,&expires,NULL)){timer_delete(timer);return -1;}

    /* Landlock cannot revoke inherited writable descriptors. Only /dev/null
     * may occupy stdio; FD4 is the pool's bounded result pipe. */
    for(int fd=0;fd<3;fd++){
        struct stat s;int flags=fcntl(fd,F_GETFL);
        if(fstat(fd,&s)||!S_ISCHR(s.st_mode)||s.st_rdev!=makedev(1,3)||flags<0||
           (flags&O_ACCMODE)!=O_RDWR||(flags&O_PATH))return -1;
    }
    struct stat output;int flags=fcntl(4,F_GETFL);
    if(fstat(4,&output)||!S_ISFIFO(output.st_mode)||flags<0||(flags&O_ACCMODE)!=O_WRONLY)return -1;
    return 0;
}
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
    fprintf(stderr,"{\"event\":\"worker_cancelled\",\"pid\":%ld,\"device\":%d,\"elapsed_ms\":%llu}\n",(long)j->pid,j->device,(unsigned long long)elapsed(j->started));
    close(j->fd);cleanup(j->scratch);memset(j,0,sizeof *j);return result<0?-1:0;
}
void worker_pool_close(WorkerPool *p){if(!p)return;for(int i=0;i<2;i++)if(p->jobs[i].pid)worker_cancel(p,i);free(p);}
static int start(WorkerPool *p,int mode,int device,const void *snapshot,size_t bytes,unsigned rows,unsigned timeout){
    if(!p||device<0||device>1||!timeout||timeout>60000||!snapshot||!bytes||bytes>sizeof(WorkerBatch))return -1;
    uint64_t now=milliseconds();if(!now)return -1;
    int slot=-1;for(int i=0;i<2;i++){if(p->jobs[i].pid&&p->jobs[i].device==device)return -1;if(!p->jobs[i].pid)slot=i;}if(slot<0)return -1;
    int input=memfd_create("cnet-frozen-cell",MFD_CLOEXEC|MFD_ALLOW_SEALING);if(input<0)return -1;
    size_t copied=0;
    while(copied<bytes){ssize_t n=write(input,(const char*)snapshot+copied,bytes-copied);if(n<0&&errno==EINTR)continue;if(n<=0){close(input);return -1;}copied+=(size_t)n;}
    if(fcntl(input,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)){close(input);return -1;}
    int descriptors[2];if(pipe2(descriptors,O_CLOEXEC)){close(input);return -1;}
    int readfd=descriptors[0],writefd=fcntl(descriptors[1],F_DUPFD_CLOEXEC,10);close(descriptors[1]);
    int infd=fcntl(input,F_DUPFD_CLOEXEC,10);close(input);int nullfd=open("/dev/null",O_RDWR|O_CLOEXEC);
    int highnull=nullfd>=0?fcntl(nullfd,F_DUPFD_CLOEXEC,10):-1;if(nullfd>=0)close(nullfd);
    int owner=(int)syscall(SYS_pidfd_open,gettid(),PIDFD_THREAD);
    int highowner=owner>=0?fcntl(owner,F_DUPFD_CLOEXEC,10):-1;if(owner>=0)close(owner);
    char scratch[128]="/tmp/cnet-gpu-worker-XXXXXX";int made=0;
    int rc=-1;if(writefd<0||infd<0||highnull<0||highowner<0)goto done;
    if(!mkdtemp(scratch))goto done;
    made=1;posix_spawn_file_actions_t actions;if(posix_spawn_file_actions_init(&actions))goto done;
    int err=posix_spawn_file_actions_adddup2(&actions,infd,3)||posix_spawn_file_actions_adddup2(&actions,writefd,4)||
        posix_spawn_file_actions_adddup2(&actions,highnull,0)||posix_spawn_file_actions_adddup2(&actions,highnull,1)||
        posix_spawn_file_actions_adddup2(&actions,highnull,2)||posix_spawn_file_actions_adddup2(&actions,highowner,5)||
        posix_spawn_file_actions_addclosefrom_np(&actions,6);
    char mode_text[16],device_text[16];snprintf(mode_text,sizeof mode_text,"%d",mode);snprintf(device_text,sizeof device_text,"%d",device);
    char temporary[160];snprintf(temporary,sizeof temporary,"TMPDIR=%s",scratch);
    char deadline_text[32];snprintf(deadline_text,sizeof deadline_text,"%llu",(unsigned long long)(now+timeout));
    char *argv[]={p->path,mode_text,device_text,scratch,deadline_text,NULL};char *env[]={"PATH=/usr/bin:/bin","LANG=C",temporary,"AMD_COMGR_CACHE=0",NULL};pid_t pid=0;
    if(!err)err=posix_spawn(&pid,p->path,&actions,NULL,argv,env);
    posix_spawn_file_actions_destroy(&actions);if(err)goto done;
    p->jobs[slot]=(Job){.pid=pid,.fd=readfd,.mode=mode,.device=device,.rows=rows,.started=now,.deadline=now+timeout};
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
    if(highowner>=0)close(highowner);
    return rc;
}
int worker_start(WorkerPool *p,int mode,int device,const CnetCoreCell *snapshot,unsigned timeout){if((mode!=WORKER_TRAIN&&mode!=WORKER_EVALUATE)||cnet_core_cell_validate(snapshot))return -1;return start(p,mode,device,snapshot,sizeof *snapshot,8,timeout);}
int worker_start_batch(WorkerPool *p,int mode,int device,const WorkerBatch *b,unsigned timeout){
    if((mode!=WORKER_BATCH_TRAIN&&mode!=WORKER_BATCH_EVALUATE)||worker_batch_validate(b))return -1;
    return start(p,mode,device,b,sizeof *b,b->rows,timeout);
}
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
    uint64_t now=milliseconds();
    if(j->used>sizeof(WorkerResult)||!now||now>=j->deadline){fprintf(stderr,"{\"event\":\"worker_limit_refused\",\"pid\":%ld,\"oversize\":%d,\"clock_failed\":%d}\n",(long)j->pid,j->used>sizeof(WorkerResult),!now);worker_cancel(p,slot);return -1;}
    int status;pid_t finished=waitpid(j->pid,&status,WNOHANG);if(!finished)return 0;
    if(finished<0&&errno==EINTR)return 0;
    /* Exit may race the first nonblocking read: drain again after reaping. */
    int read_failed=read_result(j);
    WorkerResult result={0};int valid=finished==j->pid&&!read_failed&&WIFEXITED(status)&&WEXITSTATUS(status)==0&&j->eof&&j->used==sizeof result;
    if(valid){memcpy(&result,j->bytes,sizeof result);valid=result.magic==UINT32_C(0x43575031)&&result.mode==(unsigned)j->mode&&result.device==(unsigned)j->device&&result.rows==j->rows&&result.sandboxed==1&&
        !cnet_core_cell_validate(&result.cell)&&isfinite(result.loss)&&result.loss>=0&&isfinite(result.max_error)&&result.max_error>=0&&result.max_error<1e-6f;}
    fprintf(stderr,"{\"event\":\"worker_completed\",\"pid\":%ld,\"device\":%d,\"valid\":%d,\"exit\":%d,\"signal\":%d,\"bytes\":%zu,\"elapsed_ms\":%llu}\n",(long)j->pid,j->device,valid,finished>0&&WIFEXITED(status)?WEXITSTATUS(status):-1,finished>0&&WIFSIGNALED(status)?WTERMSIG(status):0,j->used,(unsigned long long)elapsed(j->started));
    close(j->fd);cleanup(j->scratch);memset(j,0,sizeof *j);if(!valid)return -1;*out=result;return 1;
}
#ifdef CONTROLLER_TESTING
int worker_start_fault(WorkerPool *p,int device,int fault,unsigned timeout){CnetCoreCell zero={{0}};if(fault<1||fault>3)return -1;return start(p,100+fault,device,&zero,sizeof zero,8,timeout);}
#endif
