#define _GNU_SOURCE
#include "cnet_learning_sandbox.h"
#include "cnet_capsule_snapshot.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#if defined(__linux__) && defined(__x86_64__)
static int unprivileged(void) {
    uid_t uid,euid,suid;gid_t gid,egid,sgid,groups[128];
    struct __user_cap_header_struct header={_LINUX_CAPABILITY_VERSION_3,0};
    struct __user_cap_data_struct caps[2]={{0}};
    if(getresuid(&uid,&euid,&suid)||getresgid(&gid,&egid,&sgid)||
       !uid||!gid||uid!=euid||uid!=suid||gid!=egid||gid!=sgid||
       syscall(SYS_capget,&header,caps))return 0;
    for(unsigned i=0;i<2;i++)if(caps[i].permitted||caps[i].effective||caps[i].inheritable)return 0;
    int n=getgroups(128,groups);if(n<0)return 0;
    for(int i=0;i<n;i++)if(!groups[i])return 0;
    return 1;
}
static int single_thread(void) {
    DIR *d=opendir("/proc/self/task");if(!d)return 0;
    unsigned n=0;struct dirent *e;errno=0;
    while((e=readdir(d)))if(e->d_name[0]!='.')n++;
    int error=errno;if(closedir(d))return 0;
    return !error&&n==1;
}
static int streams(void) {
    for(int fd=0;fd<3;fd++) {
        struct stat s;int flags=fcntl(fd,F_GETFL);
        if(flags<0||fstat(fd,&s))return -1;
        int access=flags&O_ACCMODE;
        if(S_ISCHR(s.st_mode)&&s.st_rdev==makedev(1,3))continue;
        if(!fd&&access==O_RDONLY&&(S_ISREG(s.st_mode)||S_ISFIFO(s.st_mode)))continue;
        if(fd&&access==O_WRONLY&&S_ISFIFO(s.st_mode))continue;
        return -1;
    }
    return 0;
}
static int empty_directory(int fd) {
    int scan=openat(fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(scan<0)return 0;
    DIR *d=fdopendir(scan);if(!d){close(scan);return 0;}
    int empty=1;struct dirent *e;errno=0;
    while((e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")){empty=0;break;}
    int error=errno;if(closedir(d))return 0;
    return empty&&!error;
}
static int limit(int which,rlim_t amount) {
    struct rlimit value={amount,amount};return setrlimit(which,&value);
}
/* Deny by default: includes every socket family, inherited-socket IPC, exec,
 * io_uring, tracing/process memory/signals, namespace/privilege/metadata changes
 * and ioctl mutation through read-only FDs. Landlock checks all writable opens
 * and directory operations below. Native x32 syscall numbers never match. */
#define ALLOW(n) BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_##n,0,1), BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)
static int process_seal(void) {
    struct sock_filter code[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_X86_64,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        /* F_SETOWN/F_SETSIG must not restore external signal authority. */
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_fcntl,0,5),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[1])),
        BPF_JUMP(BPF_JMP|BPF_JGT|BPF_K,F_SETFL,0,1),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,F_DUPFD_CLOEXEC,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        /* glibc getrlimit uses prlimit64: read our own limits, never change
         * limits or inspect another process. Check the complete NULL pointer. */
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_prlimit64,0,8),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[0])),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0,0,5),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[2])),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0,0,3),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[2])+4),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        ALLOW(read),ALLOW(readv),ALLOW(pread64),ALLOW(write),ALLOW(writev),ALLOW(pwrite64),
        ALLOW(close),ALLOW(lseek),ALLOW(fstat),ALLOW(stat),ALLOW(lstat),ALLOW(newfstatat),ALLOW(statx),
        ALLOW(open),ALLOW(openat),ALLOW(getdents),ALLOW(getdents64),ALLOW(readlink),ALLOW(readlinkat),
        ALLOW(access),ALLOW(faccessat),ALLOW(faccessat2),
        ALLOW(mkdir),ALLOW(mkdirat),ALLOW(rmdir),ALLOW(unlink),ALLOW(unlinkat),
        ALLOW(rename),ALLOW(renameat),ALLOW(renameat2),ALLOW(link),ALLOW(linkat),
        ALLOW(fsync),ALLOW(fdatasync),ALLOW(flock),ALLOW(truncate),ALLOW(ftruncate),
        ALLOW(mmap),ALLOW(mprotect),ALLOW(munmap),ALLOW(mremap),ALLOW(brk),ALLOW(msync),
        ALLOW(rt_sigaction),ALLOW(rt_sigprocmask),ALLOW(rt_sigreturn),ALLOW(sigaltstack),
        ALLOW(futex),ALLOW(sched_yield),ALLOW(sched_getaffinity),
        ALLOW(getpid),ALLOW(getppid),ALLOW(gettid),ALLOW(getuid),ALLOW(geteuid),
        ALLOW(getgid),ALLOW(getegid),ALLOW(getgroups),ALLOW(getcwd),ALLOW(chdir),ALLOW(fchdir),
        ALLOW(clock_gettime),ALLOW(clock_getres),ALLOW(clock_nanosleep),ALLOW(nanosleep),
        ALLOW(gettimeofday),ALLOW(times),ALLOW(getrusage),ALLOW(getrlimit),
        ALLOW(getrandom),ALLOW(uname),ALLOW(sysinfo),ALLOW(dup),ALLOW(dup2),ALLOW(dup3),
        ALLOW(poll),ALLOW(ppoll),ALLOW(select),ALLOW(pselect6),ALLOW(restart_syscall),
        ALLOW(exit),ALLOW(exit_group),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM)
    };
    struct sock_fprog filter={.len=sizeof code/sizeof *code,.filter=code};
    return syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_TSYNC,&filter)==0?0:-1;
}
int cnet_learning_sandbox_enter_for_parent(const char *output,unsigned cpu,
    size_t address,size_t file,int expected_parent) {
    if(expected_parent<=1||getppid()!=expected_parent||
       !output||output[0]!='/'||!cpu||cpu>120||address<16UL*1024*1024||
       address>2UL*1024*1024*1024||!file||file>16UL*1024*1024||
       !unprivileged()||!single_thread()||streams())return -1;
    /* Close all inherited authority before opening our own pinned root. */
    if(syscall(SYS_close_range,3U,~0U,0))return -1;
    int root=cnet_capsule_owner_directory(output,1);if(root<0)return -1;
    int rc=-1,rules=-1;
    if(!empty_directory(root))goto done;
    long abi=syscall(SYS_landlock_create_ruleset,NULL,0,LANDLOCK_CREATE_RULESET_VERSION);
    if(abi<3)goto done;
    uint64_t writes=LANDLOCK_ACCESS_FS_WRITE_FILE|LANDLOCK_ACCESS_FS_REMOVE_DIR|
        LANDLOCK_ACCESS_FS_REMOVE_FILE|LANDLOCK_ACCESS_FS_MAKE_CHAR|LANDLOCK_ACCESS_FS_MAKE_DIR|
        LANDLOCK_ACCESS_FS_MAKE_REG|LANDLOCK_ACCESS_FS_MAKE_SOCK|LANDLOCK_ACCESS_FS_MAKE_FIFO|
        LANDLOCK_ACCESS_FS_MAKE_BLOCK|LANDLOCK_ACCESS_FS_MAKE_SYM|LANDLOCK_ACCESS_FS_REFER|
        LANDLOCK_ACCESS_FS_TRUNCATE;
    struct landlock_ruleset_attr attr={.handled_access_fs=writes};
    rules=(int)syscall(SYS_landlock_create_ruleset,&attr,sizeof attr,0);if(rules<0)goto done;
    struct landlock_path_beneath_attr rule={.parent_fd=root,.allowed_access=
        LANDLOCK_ACCESS_FS_WRITE_FILE|LANDLOCK_ACCESS_FS_TRUNCATE|LANDLOCK_ACCESS_FS_MAKE_REG|
        LANDLOCK_ACCESS_FS_MAKE_DIR|LANDLOCK_ACCESS_FS_REMOVE_FILE|LANDLOCK_ACCESS_FS_REMOVE_DIR|
        LANDLOCK_ACCESS_FS_REFER};
    if(syscall(SYS_landlock_add_rule,rules,LANDLOCK_RULE_PATH_BENEATH,&rule,0)||
       limit(RLIMIT_CPU,cpu)||limit(RLIMIT_AS,address)||limit(RLIMIT_FSIZE,file)||
       limit(RLIMIT_CORE,0)||limit(RLIMIT_NOFILE,256)||limit(RLIMIT_MEMLOCK,0)||
       getppid()!=expected_parent||prctl(PR_SET_PDEATHSIG,SIGKILL,0,0,0)||getppid()!=expected_parent||
       prctl(PR_SET_DUMPABLE,0,0,0,0)||prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0))goto done;
    if(clearenv())goto done;
    umask(077);
    if(syscall(SYS_landlock_restrict_self,rules,0))goto done;
    close(rules);rules=-1;close(root);root=-1;
    rc=process_seal();
done:
    if(rules>=0)close(rules);
    if(root>=0)close(root);
    return rc;
}
#else
int cnet_learning_sandbox_enter_for_parent(const char *output,unsigned cpu,
    size_t address,size_t file,int expected_parent) {
    (void)output;(void)cpu;(void)address;(void)file;(void)expected_parent;return -1;
}
#endif
int cnet_learning_sandbox_enter(const char *output,unsigned cpu,size_t address,size_t file) {
    return cnet_learning_sandbox_enter_for_parent(output,cpu,address,file,getppid());
}
