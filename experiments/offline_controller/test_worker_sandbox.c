#define _GNU_SOURCE
#include "worker_sandbox.h"
#include "offline.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(void){
    char dir[]="/tmp/cnet-worker-seal-XXXXXX";assert(mkdtemp(dir));char path[256];snprintf(path,sizeof path,"%s/active.core",dir);
    int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);assert(fd>=0);assert(write(fd,"sealed",6)==6);assert(!close(fd));
    assert(!worker_filesystem_seal(NULL));assert(!offline_seal(-1));assert(!worker_process_seal());assert(!offline_negative_test());
    errno=0;assert(open(path,O_RDWR)<0&&errno==EACCES);
    int readonly=open(path,O_RDONLY);assert(readonly>=0);assert(truncate(path,0)!=0);assert(unlink(path)!=0);assert(chmod(path,0666)!=0);
    int flags=FS_NODUMP_FL;errno=0;assert(ioctl(readonly,FS_IOC_SETFLAGS,&flags)<0&&errno==EPERM);
    errno=0;assert(syscall(SYS_rt_sigqueueinfo,999999999,SIGUSR1,NULL)<0&&errno==EPERM);
    errno=0;assert(syscall(SYS_rt_tgsigqueueinfo,999999999,999999999,SIGUSR1,NULL)<0&&errno==EPERM);
    char other[256];snprintf(other,sizeof other,"%s/new",dir);assert(open(other,O_CREAT|O_WRONLY,0600)<0);assert(rename(path,other)!=0);
    assert(link(path,other)!=0);assert(symlink(path,other)!=0);assert(mkdir(other,0700)!=0);
    puts("WORKER_SANDBOX_PASS network_denied=1 filesystem_mutation_denied=1 metadata_denied=1 readonly_allowed=1");
}
