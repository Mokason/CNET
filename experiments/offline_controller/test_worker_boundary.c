#define _GNU_SOURCE
#include "worker.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int jump_clock,fail_clock,fail_timer,fail_arm;
int __real_clock_gettime(clockid_t,struct timespec *);
int __wrap_clock_gettime(clockid_t clock,struct timespec *out){
    if(fail_clock&&clock==CLOCK_BOOTTIME){errno=EIO;return -1;}
    int rc=__real_clock_gettime(clock,out);
    if(!rc&&jump_clock&&clock==CLOCK_BOOTTIME)out->tv_sec+=120;
    return rc;
}
int __real_timer_create(clockid_t,struct sigevent *,timer_t *);
int __wrap_timer_create(clockid_t clock,struct sigevent *event,timer_t *timer){
    if(fail_timer){errno=ENOTSUP;return -1;}
    assert(clock==CLOCK_BOOTTIME&&event->sigev_notify==SIGEV_SIGNAL&&event->sigev_signo==SIGKILL);
    return __real_timer_create(clock,event,timer);
}
int __real_timer_settime(timer_t,int,const struct itimerspec *,struct itimerspec *);
int __wrap_timer_settime(timer_t timer,int flags,const struct itimerspec *value,struct itimerspec *old){
    if(fail_arm){errno=EIO;return -1;}
    assert(flags==TIMER_ABSTIME);return __real_timer_settime(timer,flags,value,old);
}
static void delay(void){struct timespec t={0,1000000};nanosleep(&t,NULL);}
static int finish(WorkerPool *p,int slot){
    WorkerResult r;int rc;do{rc=worker_poll(p,slot,&r);if(!rc)delay();}while(!rc);return rc;
}
static int fixture(int argc,char **argv){
    CnetCoreCell cell;assert(pread(3,&cell,sizeof cell,0)==sizeof cell);
    /* Test-only scheduling point: stop before entering the production guard. */
    if(cell.weight[0]==2)assert(!raise(SIGSTOP));
    if(argc!=5||worker_guard_enter(argv[4]))return 3;
    if(cell.weight[0]==1)assert(!raise(SIGSTOP));
    if(cell.weight[0]==1||cell.weight[0]==2)for(;;)pause();
    const char marker[]="UNTRUSTED_WORKER_STDERR\n";
    assert(write(2,marker,sizeof marker-1)==sizeof marker-1);
    WorkerResult r={.magic=UINT32_C(0x43575031),.mode=WORKER_EVALUATE,
        .device=(unsigned)atoi(argv[2]),.rows=8,.cell=cell,.sandboxed=1};
    assert(write(4,&r,sizeof r)==sizeof r);return 0;
}
static pid_t child_pid(void){
    char path[96];snprintf(path,sizeof path,"/proc/self/task/%ld/children",(long)gettid());
    FILE *f=fopen(path,"r");long pid=0;assert(f&&fscanf(f,"%ld",&pid)==1);fclose(f);return (pid_t)pid;
}
static int failures;
static void check(int ok,const char *name){
    printf("WORKER_BOUNDARY_%s %s\n",ok?"PASS":"RED",name);fflush(stdout);failures+=!ok;
}
typedef struct {const char *path;int ready[2],done[2],slot;pid_t pid;WorkerPool *pool;int before_guard;} Owner;
static void *thread_owner(void *arg){
    Owner *o=arg;CnetCoreCell cell={{0}};cell.weight[0]=o->before_guard?2:1;
    o->pool=worker_pool_open(o->path);assert(o->pool);
    o->slot=worker_start(o->pool,WORKER_EVALUATE,0,&cell,60000);assert(o->slot>=0);o->pid=child_pid();
    char byte=0;assert(write(o->ready[1],&byte,1)==1);assert(read(o->done[0],&byte,1)==1);return NULL;
}
static void owner_thread_test(const char *path,int before_guard){
    Owner o={.path=path,.before_guard=before_guard};assert(!pipe(o.ready)&&!pipe(o.done));
    pthread_t thread;assert(!pthread_create(&thread,NULL,thread_owner,&o));char byte;assert(read(o.ready[0],&byte,1)==1);
    int status;assert(waitpid(o.pid,&status,WUNTRACED)==o.pid&&WIFSTOPPED(status));
    assert(write(o.done[1],&byte,1)==1&&!pthread_join(thread,NULL));
    if(before_guard)assert(!kill(o.pid,SIGCONT));
    siginfo_t info={0};for(unsigned i=0;i<500&&!info.si_pid;i++){
        assert(!waitid(P_PID,(id_t)o.pid,&info,WEXITED|WNOHANG|WNOWAIT));if(!info.si_pid)delay();
    }
    check(info.si_pid==o.pid&&((info.si_code==CLD_KILLED&&info.si_status==SIGKILL)||
        (before_guard&&info.si_code==CLD_EXITED&&info.si_status==3)),
        before_guard?"spawning_thread_dies_before_guard":"spawning_thread_dies_after_guard");
    /* Test-only cleanup takes over from the now-dead owner, without races. */
    assert(!worker_cancel(o.pool,o.slot));worker_pool_close(o.pool);
    close(o.ready[0]);close(o.ready[1]);close(o.done[0]);close(o.done[1]);
}
static int high_fd(int fd){assert(fd>=0);int high=fcntl(fd,F_DUPFD_CLOEXEC,20);assert(high>=0);close(fd);return high;}
static void direct_guard_test(int fault,const char *name){
    int owner=high_fd((int)syscall(SYS_pidfd_open,gettid(),O_EXCL));
    int null=high_fd(open("/dev/null",O_RDWR|O_CLOEXEC));
    int sentinel=high_fd(memfd_create("worker-direct-sentinel",MFD_CLOEXEC));
    int ends[2];assert(!pipe2(ends,O_CLOEXEC));int read_end=high_fd(ends[0]),write_end=high_fd(ends[1]);
    struct timespec now;assert(!clock_gettime(CLOCK_BOOTTIME,&now));char deadline[32];
    snprintf(deadline,sizeof deadline,"%llu",(unsigned long long)now.tv_sec*1000+(unsigned)now.tv_nsec/1000000+5000);
    pid_t pid=fork();assert(pid>=0);
    if(!pid){
        for(int i=0;i<3;i++)assert(dup2(null,i)==i);
        assert(dup2(write_end,4)==4&&dup2(owner,5)==5);
        if(fault>=1&&fault<=3)assert(dup2(sentinel,fault-1)==fault-1);
        if(fault==4)assert(dup2(sentinel,4)==4);
        if(fault==5)assert(dup2(read_end,4)==4);
        if(fault==6)assert(dup2(sentinel,5)==5);
        if(fault==7)fail_clock=1;
        if(fault==8)fail_timer=1;
        if(fault==9)fail_arm=1;
        if(fault==10)strcpy(deadline,"0");
        if(fault==11)strcpy(deadline,"18446744073709551615");
        int rc=worker_guard_enter(deadline);
        _exit(fault?(rc<0?0:90):(rc==0&&fcntl(sentinel,F_GETFD)<0&&errno==EBADF?0:91));
    }
    int status;assert(waitpid(pid,&status,0)==pid);check(WIFEXITED(status)&&WEXITSTATUS(status)==0,name);
    close(owner);close(null);close(sentinel);close(read_end);close(write_end);
}
static void owner_process_test(const char *path,int before_guard){
    assert(!prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0));int ready[2];assert(!pipe(ready));
    pid_t owner=fork();assert(owner>=0);
    if(!owner){
        close(ready[0]);WorkerPool *p=worker_pool_open(path);CnetCoreCell cell={{0}};cell.weight[0]=before_guard?2:1;
        assert(p&&worker_start(p,WORKER_EVALUATE,0,&cell,60000)>=0);pid_t pid=child_pid();int status;
        assert(waitpid(pid,&status,WUNTRACED)==pid&&WIFSTOPPED(status));
        assert(write(ready[1],&pid,sizeof pid)==sizeof pid);for(;;)pause();
    }
    close(ready[1]);pid_t pid;assert(read(ready[0],&pid,sizeof pid)==sizeof pid);close(ready[0]);
    char proc[96],args[8192]={0};snprintf(proc,sizeof proc,"/proc/%ld/cmdline",(long)pid);
    int fd=open(proc,O_RDONLY);assert(fd>=0&&read(fd,args,sizeof args-1)>0);close(fd);
    char *scratch=args;for(int i=0;i<3;i++)scratch+=strlen(scratch)+1;
    assert(!strncmp(scratch,"/tmp/cnet-gpu-worker-",21));
    assert(!kill(owner,SIGKILL)&&waitpid(owner,NULL,0)==owner);
    if(before_guard)assert(!kill(pid,SIGCONT));
    siginfo_t info={0};for(unsigned i=0;i<500&&!info.si_pid;i++){
        assert(!waitid(P_PID,(id_t)pid,&info,WEXITED|WNOHANG|WNOWAIT));if(!info.si_pid)delay();
    }
    check(info.si_pid==pid&&((info.si_code==CLD_KILLED&&info.si_status==SIGKILL)||
        (before_guard&&info.si_code==CLD_EXITED&&info.si_status==3)),
        before_guard?"spawning_process_dies_before_guard":"spawning_process_dies_after_guard");
    if(!info.si_pid)assert(!kill(pid,SIGKILL));
    assert(waitpid(pid,NULL,0)==pid&&!rmdir(scratch));assert(!prctl(PR_SET_CHILD_SUBREAPER,0,0,0,0));
}
int main(int argc,char **argv){
    if(argc>=4)return fixture(argc,argv);
    assert(argc==1);char executable[4096];assert(realpath(argv[0],executable));
    WorkerPool *p=worker_pool_open(executable);assert(p);CnetCoreCell cell={{0}};
    int log=memfd_create("worker-boundary-stderr",MFD_CLOEXEC),saved=dup(2);assert(log>=0&&saved>=0);
    assert(dup2(log,2)==2);int slot=worker_start(p,WORKER_EVALUATE,0,&cell,5000);
    assert(slot>=0&&finish(p,slot)==1);assert(dup2(saved,2)==2);close(saved);
    char output[4096]={0};assert(pread(log,output,sizeof output-1,0)>0);close(log);
    check(!strstr(output,"UNTRUSTED_WORKER_STDERR"),"inherited_stderr_is_not_writable");

    cell.weight[0]=1;slot=worker_start(p,WORKER_EVALUATE,0,&cell,60000);assert(slot>=0);
    jump_clock=1;WorkerResult result;int rc=worker_poll(p,slot,&result);jump_clock=0;
    check(rc<0,"suspend_inclusive_pool_deadline");if(!rc)assert(!worker_cancel(p,slot));
    fail_clock=1;slot=worker_start(p,WORKER_EVALUATE,0,&cell,60000);fail_clock=0;
    check(slot<0,"clock_failure_refuses_start");if(slot>=0)assert(!worker_cancel(p,slot));
    slot=worker_start(p,WORKER_EVALUATE,0,&cell,60000);assert(slot>=0);
    log=memfd_create("worker-clock-failure-log",MFD_CLOEXEC);saved=dup(2);assert(log>=0&&saved>=0&&dup2(log,2)==2);
    fail_clock=1;rc=worker_poll(p,slot,&result);fail_clock=0;
    assert(dup2(saved,2)==2);close(saved);memset(output,0,sizeof output);
    assert(pread(log,output,sizeof output-1,0)>0);close(log);
    check(rc<0,"clock_failure_refuses_poll");if(!rc)assert(!worker_cancel(p,slot));
    check(strstr(output,"\"clock_failed\":1")&&strstr(output,"\"elapsed_ms\":0"),"clock_failure_diagnostic_is_bounded");

    slot=worker_start(p,WORKER_EVALUATE,0,&cell,100);assert(slot>=0);pid_t pid=child_pid();
    siginfo_t info={0};for(unsigned i=0;i<1000&&!info.si_pid;i++){
        assert(!waitid(P_PID,(id_t)pid,&info,WEXITED|WNOHANG|WNOWAIT));if(!info.si_pid)delay();
    }
    check(info.si_pid==pid&&info.si_code==CLD_KILLED&&info.si_status==SIGKILL,"deadline_without_parent_polling");
    assert(!worker_cancel(p,slot));worker_pool_close(p);
    owner_thread_test(executable,0);owner_thread_test(executable,1);owner_process_test(executable,0);owner_process_test(executable,1);
    const char *names[]={"direct_guard_closes_extra_fds","direct_stdin_file_refused","direct_stdout_file_refused",
        "direct_stderr_file_refused","direct_result_file_refused","direct_result_read_end_refused",
        "direct_missing_parent_pidfd_refused","direct_clock_failure_refused","direct_timer_failure_refused",
        "direct_timer_arm_failure_refused","direct_expired_deadline_refused","direct_oversize_deadline_refused"};
    for(unsigned i=0;i<sizeof names/sizeof *names;i++)direct_guard_test((int)i,names[i]);
    return failures?1:0;
}
