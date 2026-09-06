#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cnet_learning_sandbox.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <linux/fs.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static void check(int yes,const char *name) {
    printf("LEARNING_SANDBOX_%s %s\n",yes?"PASS":"RED",name);
    if(!yes)failures++;
}
static void worker_stdio(void) {
    int fd=open("/dev/null",O_RDWR);
    if(fd<0||dup2(fd,0)<0||dup2(fd,1)<0||dup2(fd,2)<0)_exit(90);
    if(fd>2)close(fd);
}
static int enter(const char *output) {
    return cnet_learning_sandbox_enter(output,30,512UL*1024*1024,16UL*1024*1024);
}
static int wait_child(pid_t pid) {
    int status;if(waitpid(pid,&status,0)!=pid)return 0;
    return WIFEXITED(status)&&WEXITSTATUS(status)==0;
}
static int denied(int rc) { return rc<0&&(errno==EPERM||errno==EACCES||errno==EXDEV); }
static int attacks(const char *output,const char *policy,const char *socketpath) {
    char a[320],b[320];snprintf(a,sizeof a,"%s/item",output);
    snprintf(b,sizeof b,"%s/renamed",output);
    int fd=open(a,O_CREAT|O_EXCL|O_RDWR,0666);
    if(fd<0||write(fd,"candidate",9)!=9||fsync(fd)||close(fd)||rename(a,b))return 1;
    if(link(b,a)||unlink(a))return 2;
    int directory=open(output,O_RDONLY|O_DIRECTORY);
    if(directory<0||fsync(directory)||close(directory))return 3;
    /* Export can rename/link internally, but cannot import an outside inode. */
    if(!denied(link(policy,a))||!denied(rename(policy,a))||!denied(unlink(policy)))return 4;
    if(!denied(symlink(policy,a))||!denied(chmod(policy,0777)))return 5;
    fd=open(policy,O_RDONLY);if(fd<0)return 6;
    int flags=FS_IMMUTABLE_FL;
    if(!denied(ioctl(fd,FS_IOC_SETFLAGS,&flags))||!denied(fchmod(fd,0777))||
       !denied(fchown(fd,geteuid(),getegid()))||!denied(fcntl(fd,F_SETOWN,getppid())))return 7;
    close(fd);
    /* A socket syscall must fail for network AND owner control IPC. */
    if(!denied(socket(AF_UNIX,SOCK_STREAM,0))||!denied(socket(AF_INET,SOCK_STREAM,0)))return 8;
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    snprintf(addr.sun_path,sizeof addr.sun_path,"%s",socketpath);
    if(!denied(connect(-1,(struct sockaddr *)&addr,sizeof addr)))return 9;
    if(!denied(kill(getppid(),0))||!denied(syscall(SYS_process_vm_readv,getppid(),NULL,0,NULL,0,0))||
       !denied(syscall(SYS_pidfd_open,getppid(),0))||!denied(syscall(SYS_io_uring_setup,1,NULL)))return 10;
    if(!denied(syscall(SYS_execve,"/bin/true",NULL,NULL))||!denied(fork())||
       !denied(prctl(PR_SET_PDEATHSIG,0,0,0,0)))return 11;
    if(unlink(b))return 12;
    return 0;
}
static int parent_death(const char *output) {
    if(prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0))return 0;
    pid_t parent=fork();
    if(!parent) {
        int ready[2];if(pipe(ready))_exit(90);
        pid_t original=getpid();
        pid_t child=fork();if(child<0)_exit(90);
        if(!child) {
            worker_stdio();if(dup2(ready[1],1)<0||
                cnet_learning_sandbox_enter_for_parent(output,30,512UL<<20,16UL<<20,original))_exit(91);
            if(write(1,"ready",5)!=5)_exit(92);
            for(;;)sleep(1);
        }
        close(ready[1]);char data[5];
        _exit(read(ready[0],data,sizeof data)==sizeof data?0:93);
    }
    int ok=parent>0&&wait_child(parent),status=0;
    pid_t child=waitpid(-1,&status,0);
    if(prctl(PR_SET_CHILD_SUBREAPER,0,0,0,0))return 0;
    return ok&&child>0&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL;
}
static int adopted_before_seal(const char *output) {
    if(prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0))return 0;
    pid_t launcher=fork();
    if(!launcher) {
        /* Original launcher identity is captured before the worker exists. */
        pid_t original=getpid();
        pid_t child=fork();if(child<0)_exit(90);
        if(!child) {
            worker_stdio();
            while(getppid()==original)usleep(1000);
            if(getppid()<=1)_exit(92); /* Must exercise adoption by a subreaper. */
            int rc=cnet_learning_sandbox_enter_for_parent(output,30,512UL<<20,16UL<<20,original);
            _exit(rc!=0?0:1);
        }
        _exit(0);
    }
    int ok=launcher>0&&wait_child(launcher),status=0;
    pid_t child=waitpid(-1,&status,0);
    if(prctl(PR_SET_CHILD_SUBREAPER,0,0,0,0))return 0;
    return ok&&child>0&&WIFEXITED(status)&&WEXITSTATUS(status)==0;
}
int main(void) {
    char root[]="/tmp/cnet-learning-sandbox-XXXXXX",output[256],policy[256],file[256],socketpath[108];
    if(!mkdtemp(root))return 2;
    snprintf(output,sizeof output,"%s/output",root);
    snprintf(policy,sizeof policy,"%s/policy",root);
    snprintf(file,sizeof file,"%s/output/artifact",root);
    snprintf(socketpath,sizeof socketpath,"%s/control.sock",root);
    if(mkdir(output,0700))return 2;
    int policyfd=open(policy,O_CREAT|O_RDWR|O_EXCL,0600);
    if(policyfd<0||write(policyfd,"owner",5)!=5)return 2;
    pid_t pid=fork();
    if(!pid) {
        worker_stdio();if(enter(output))_exit(91);
        int fd=open(policy,O_WRONLY|O_TRUNC);
        if(fd>=0){close(fd);_exit(1);}
        _exit(errno==EACCES||errno==EPERM?0:2);
    }
    check(pid>0&&wait_child(pid),"policy_write_denied");
    pid=fork();
    if(!pid) {
        worker_stdio();if(enter(output))_exit(91);
        int fd=socket(AF_UNIX,SOCK_STREAM,0);
        if(fd>=0){close(fd);_exit(1);}
        _exit(errno==EPERM?0:2);
    }
    check(pid>0&&wait_child(pid),"owner_control_socket_denied");
    pid=fork();
    if(!pid) {
        worker_stdio();if(enter(output))_exit(91);
        _exit(write(policyfd,"bad",3)<0&&errno==EBADF?0:1);
    }
    check(pid>0&&wait_child(pid),"inherited_policy_descriptor_closed");
    pid=fork();
    if(!pid) {
        worker_stdio();
        if(cnet_learning_sandbox_enter(output,0,512UL<<20,16UL<<20)==0||
           cnet_learning_sandbox_enter(output,121,512UL<<20,16UL<<20)==0||
           cnet_learning_sandbox_enter(output,30,15UL<<20,16UL<<20)==0||
           cnet_learning_sandbox_enter(output,30,(2UL<<30)+1,16UL<<20)==0||
           cnet_learning_sandbox_enter(output,30,512UL<<20,0)==0||
           cnet_learning_sandbox_enter(output,30,512UL<<20,(16UL<<20)+1)==0)_exit(1);
        _exit(0);
    }
    check(pid>0&&wait_child(pid),"invalid_resource_limits_refused");
    pid=fork();
    if(!pid) {
        worker_stdio();if(enter(output))_exit(91);
        int resources[]={RLIMIT_CPU,RLIMIT_AS,RLIMIT_FSIZE,RLIMIT_CORE,RLIMIT_NOFILE,RLIMIT_MEMLOCK};
        rlim_t amounts[]={30,512UL<<20,16UL<<20,0,256,0};
        for(unsigned i=0;i<sizeof resources/sizeof *resources;i++) {
            struct rlimit value;
            if(getrlimit(resources[i],&value)||value.rlim_cur!=amounts[i]||value.rlim_max!=amounts[i])_exit(1);
        }
        void *large=mmap(NULL,513UL<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        _exit(large==MAP_FAILED&&errno==ENOMEM?0:2);
    }
    check(pid>0&&wait_child(pid),"hard_resource_limits_enforced");
    pid_t expected=getpid();
    pid=fork();
    if(!pid) {
        worker_stdio();
        int invalid[]={-1,0,1,getpid()};
        for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++)
            if(!cnet_learning_sandbox_enter_for_parent(output,2,64UL<<20,16UL<<20,invalid[i]))_exit(1);
        _exit(0);
    }
    check(pid>0&&wait_child(pid),"invalid_or_mismatched_parent_refused");
    pid=fork();
    if(!pid) {
        worker_stdio();
        if(cnet_learning_sandbox_enter_for_parent(output,2,64UL<<20,16UL<<20,expected))_exit(91);
        struct rlimit cpu,memory;
        if(getrlimit(RLIMIT_CPU,&cpu)||getrlimit(RLIMIT_AS,&memory))_exit(2);
        _exit(cpu.rlim_cur==2&&cpu.rlim_max==2&&memory.rlim_cur==(64UL<<20)&&
              memory.rlim_max==(64UL<<20)?0:3);
    }
    check(pid>0&&wait_child(pid),"stricter_policy_limits_enforced_exactly");
    alarm(5);
    check(parent_death(output),"parent_death_kills_and_reaps_worker");
    check(adopted_before_seal(output),"original_launcher_dead_before_seal_refused");
    alarm(0);
    int control=socket(AF_UNIX,SOCK_STREAM,0);
    struct sockaddr_un address={.sun_family=AF_UNIX};
    snprintf(address.sun_path,sizeof address.sun_path,"%s",socketpath);
    if(control<0||bind(control,(struct sockaddr *)&address,sizeof address)||listen(control,1))return 2;
    pid=fork();
    if(!pid) {
        worker_stdio();if(enter(output))_exit(91);
        if(fcntl(control,F_GETFD)!=-1||errno!=EBADF)_exit(1);
        _exit(attacks(output,policy,socketpath));
    }
    check(pid>0&&wait_child(pid),"publication_and_escape_attacks");
    pid=fork();
    if(!pid) {
        worker_stdio();if(dup2(control,1)<0)_exit(92);
        _exit(enter(output)!=0?0:1);
    }
    check(pid>0&&wait_child(pid),"inherited_control_socket_stream_refused");
    pid=fork();
    if(!pid) {
        worker_stdio();if(dup2(policyfd,2)<0)_exit(92);
        _exit(enter(output)!=0?0:1);
    }
    check(pid>0&&wait_child(pid),"inherited_policy_stream_refused");
    pid=fork();
    if(!pid) {
        worker_stdio();if(chmod(output,0755))_exit(92);
        _exit(enter(output)!=0?0:1);
    }
    check(pid>0&&wait_child(pid),"nonprivate_root_refused");
    if(chmod(output,0700))return 2;
    pid=fork();
    if(!pid) {
        worker_stdio();
        char alias[300];snprintf(alias,sizeof alias,"%s/alias",root);
        if(symlink(output,alias))_exit(92);
        _exit(enter(alias)!=0?0:1);
    }
    check(pid>0&&wait_child(pid),"symlink_root_refused");
    char alias[300];snprintf(alias,sizeof alias,"%s/alias",root);unlink(alias);
    pid=fork();
    if(!pid) {
        worker_stdio();if(link(policy,file))_exit(92);
        _exit(enter(output)!=0?0:1);
    }
    check(pid>0&&wait_child(pid),"nonempty_hardlinked_root_refused");
    unlink(file);
    pid=fork();
    if(!pid) {
        worker_stdio();if(enter(output))_exit(91);
        int fd=open(file,O_WRONLY|O_CREAT|O_EXCL,0666);
        struct stat st;
        if(fd<0||write(fd,"candidate",9)!=9||fsync(fd)||fstat(fd,&st))_exit(1);
        close(fd);_exit((st.st_mode&077)==0?0:2);
    }
    check(pid>0&&wait_child(pid),"private_candidate_write_allowed");
    char original[6]={0};
    check(pread(policyfd,original,5,0)==5&&!strcmp(original,"owner"),"owner_policy_unchanged");
    close(control);unlink(socketpath);
    close(policyfd);unlink(file);unlink(policy);rmdir(output);rmdir(root);
    printf("LEARNING_SANDBOX_%s failures=%d\n",failures?"RED":"PASS",failures);
    return failures?1:0;
}
