#define _GNU_SOURCE
#include "offline.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#if !defined(__x86_64__)
#error This experiment filter must be explicitly ported before use on another ABI.
#endif
#define DENY(n) BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,__NR_##n,0,1), BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM)
int offline_seal(int allowed) {
    if (allowed>=0) {
        struct sockaddr_in peer; socklen_t len=sizeof peer;
        if (getpeername(allowed,(struct sockaddr *)&peer,&len) || len!=sizeof peer ||
            peer.sin_family!=AF_INET || ntohs(peer.sin_port)!=8092 ||
            ntohl(peer.sin_addr.s_addr)!=INADDR_LOOPBACK) return -1;
    }
    DIR *dir=opendir("/proc/self/fd");
    if (!dir) return -1;
    struct dirent *e; int unsafe=0;
    while ((e=readdir(dir))) {
        char *end; long fd=strtol(e->d_name,&end,10);
        if (*end || fd<0 || fd==allowed) continue;
        int kind; socklen_t len=sizeof kind;
        if (!getsockopt((int)fd,SOL_SOCKET,SO_TYPE,&kind,&len)) unsafe=1;
    }
    closedir(dir); if (unsafe) return -1;
    struct sock_filter code[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_X86_64,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,0x40000000,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        DENY(socket),DENY(socketpair),DENY(connect),DENY(bind),DENY(listen),
        DENY(accept),DENY(accept4),DENY(sendto),DENY(sendmsg),DENY(sendmmsg),
        DENY(recvfrom),DENY(recvmsg),DENY(recvmmsg),
        DENY(io_uring_setup),DENY(io_uring_register),DENY(io_uring_enter),
        DENY(execve),DENY(execveat),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)
    };
    struct sock_fprog filter={.len=sizeof code/sizeof *code,.filter=code};
    if (prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)) return -1;
    return syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_TSYNC,&filter)==0 ? 0 : -1;
}
int offline_negative_test(void) {
    errno=0; if (socket(AF_INET,SOCK_STREAM,0)!=-1 || errno!=EPERM) return -1;
    errno=0; if (socket(AF_UNIX,SOCK_STREAM,0)!=-1 || errno!=EPERM) return -1;
    errno=0; if (syscall(SYS_io_uring_setup,0,NULL)!=-1 || errno!=EPERM) return -1;
    errno=0; if (connect(-1,NULL,0)!=-1 || errno!=EPERM) return -1;
    return 0;
}
