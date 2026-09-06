/* Test-only interposition: mutate a private fixture immediately AFTER the
 * first real successful live check. No production fault hook or bypass. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int cnet_capsule_evidence_fresh(const void *e,const char *root,char *error,size_t cap) {
    static int changed;
    int (*real_check)(const void *,const char *,char *,size_t);
    *(void **)(&real_check)=dlsym(RTLD_NEXT,"cnet_capsule_evidence_fresh");
    if(!real_check)return -1;
    int rc=real_check(e,root,error,cap);
    if(!rc&&!changed) {
        changed=1;
        const char *path=getenv("CNET_TEST_MUTATE_SOURCE");
        if(!path)return -1;
        int fd=open(path,O_WRONLY|O_APPEND|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        struct stat st;
        if(fd<0)return -1;
        int ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&st.st_nlink==1&&write(fd,"\n",1)==1;
        if(close(fd))ok=0;
        if(!ok)return -1;
    }
    return rc;
}
