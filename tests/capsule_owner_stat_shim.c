/* Test-only synthetic UID metadata, never changes real filesystem ownership. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
int fstat(int fd,struct stat *st) {
    int (*real_stat)(int,struct stat *);
    *(void **)(&real_stat)=dlsym(RTLD_NEXT,"fstat");
    if(!real_stat)return -1;
    int rc=real_stat(fd,st);const char *path=getenv("CNET_TEST_FOREIGN_ANCESTOR");
    struct stat target;
    if(!rc&&path&&!lstat(path,&target)&&st->st_dev==target.st_dev&&st->st_ino==target.st_ino)
        st->st_uid=geteuid()+1;
    return rc;
}
