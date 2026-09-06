#define _GNU_SOURCE
#include "worker_sandbox.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
#if !defined(__x86_64__)
#error Worker sandbox must be explicitly ported to another ABI.
#endif
int worker_filesystem_seal(const char *scratch){
    DIR *tasks=opendir("/proc/self/task");if(!tasks)return -1;
    unsigned threads=0;struct dirent *entry;while((entry=readdir(tasks)))if(entry->d_name[0]!='.')threads++;
    closedir(tasks);if(threads!=1)return -1;
    int abi=(int)syscall(SYS_landlock_create_ruleset,NULL,0,LANDLOCK_CREATE_RULESET_VERSION);if(abi<3)return -1;
    struct landlock_ruleset_attr rules={.handled_access_fs=LANDLOCK_ACCESS_FS_WRITE_FILE|LANDLOCK_ACCESS_FS_REMOVE_DIR|
        LANDLOCK_ACCESS_FS_REMOVE_FILE|LANDLOCK_ACCESS_FS_MAKE_CHAR|LANDLOCK_ACCESS_FS_MAKE_DIR|LANDLOCK_ACCESS_FS_MAKE_REG|
        LANDLOCK_ACCESS_FS_MAKE_SOCK|LANDLOCK_ACCESS_FS_MAKE_FIFO|LANDLOCK_ACCESS_FS_MAKE_BLOCK|LANDLOCK_ACCESS_FS_MAKE_SYM|
        LANDLOCK_ACCESS_FS_REFER|LANDLOCK_ACCESS_FS_TRUNCATE};
    int fd=(int)syscall(SYS_landlock_create_ruleset,&rules,sizeof rules,0);if(fd<0)return -1;
    const char *paths[]={"/dev/kfd","/dev/dri","/dev/null"};int rc=-1;
    for(unsigned i=0;i<3;i++){
        int device=open(paths[i],O_PATH|O_CLOEXEC|O_NOFOLLOW);if(device<0)goto done;
        struct landlock_path_beneath_attr rule={.allowed_access=LANDLOCK_ACCESS_FS_WRITE_FILE,.parent_fd=device};
        int status=(int)syscall(SYS_landlock_add_rule,fd,LANDLOCK_RULE_PATH_BENEATH,&rule,0);close(device);if(status)goto done;
    }
    if(scratch){
        int path=open(scratch,O_PATH|O_CLOEXEC|O_NOFOLLOW|O_DIRECTORY);struct stat s;
        if(path<0)goto done;
        int invalid=fstat(path,&s)||s.st_uid!=geteuid()||(s.st_mode&077);
        struct landlock_path_beneath_attr rule={.allowed_access=LANDLOCK_ACCESS_FS_WRITE_FILE|LANDLOCK_ACCESS_FS_TRUNCATE|
            LANDLOCK_ACCESS_FS_MAKE_REG|LANDLOCK_ACCESS_FS_MAKE_DIR|LANDLOCK_ACCESS_FS_REMOVE_FILE|LANDLOCK_ACCESS_FS_REMOVE_DIR|LANDLOCK_ACCESS_FS_REFER,.parent_fd=path};
        if(!invalid)invalid=(int)syscall(SYS_landlock_add_rule,fd,LANDLOCK_RULE_PATH_BENEATH,&rule,0);
        close(path);if(invalid)goto done;
    }
    if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0))goto done;
    rc=(int)syscall(SYS_landlock_restrict_self,fd,0);
done:close(fd);return rc;
}
#define DENY(n) BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,__NR_##n,0,1),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM)
int worker_process_seal(void){
    struct sock_filter code[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_X86_64,1,0),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,0x40000000,0,1),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        DENY(chmod),DENY(fchmod),DENY(fchmodat),DENY(chown),DENY(fchown),DENY(lchown),DENY(fchownat),
        DENY(setxattr),DENY(lsetxattr),DENY(fsetxattr),DENY(removexattr),DENY(lremovexattr),DENY(fremovexattr),
        DENY(utime),DENY(utimes),DENY(futimesat),DENY(utimensat),
        DENY(ptrace),DENY(process_vm_writev),DENY(process_vm_readv),DENY(pidfd_getfd),
        DENY(kill),DENY(tkill),DENY(tgkill),DENY(pidfd_send_signal),
        DENY(rt_sigqueueinfo),DENY(rt_tgsigqueueinfo),DENY(prlimit64),
        DENY(setpriority),DENY(ioprio_set),DENY(sched_setaffinity),DENY(sched_setscheduler),DENY(sched_setparam),DENY(sched_setattr),
        DENY(process_madvise),DENY(process_mrelease),
        DENY(clone),DENY(clone3),DENY(fork),DENY(vfork),
        DENY(mount),DENY(umount2),DENY(pivot_root),DENY(unshare),DENY(setns),
        DENY(bpf),DENY(perf_event_open),DENY(userfaultfd),DENY(open_by_handle_at),
        /* fchmodat2 (x86_64 452) is newer than the build's syscall headers. */
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,452,0,1),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        /* Only DRM ('d') and KFD ('K') ioctl namespaces are needed by HIP.
         * Generic filesystem ioctls can mutate metadata through read-only FDs. */
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,__NR_ioctl,0,5),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[1])),
        BPF_STMT(BPF_ALU|BPF_AND|BPF_K,0xff00),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,'d'<<8,2,0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,'K'<<8,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)
    };
    struct sock_fprog filter={.len=sizeof code/sizeof *code,.filter=code};
    if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)||prctl(PR_SET_DUMPABLE,0,0,0,0))return -1;
    return syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_TSYNC,&filter)==0?0:-1;
}
