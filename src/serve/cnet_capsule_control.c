#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cnet_capsule_control.h"
#include "cnet_capsule_snapshot.h"
#include "cnetd_protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

struct CnetCapsuleControl {
    CnetCapsuleStore *store;
    int fd,parent,lock,bound;
    char name[97];
    dev_t device;ino_t inode;
};
static int name_ok(const char *s) {
    if(!s||!*s||*s=='.'||strlen(s)>80)return 0;
    for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||
       (*s>='0'&&*s<='9')||*s=='_'||*s=='-'||*s=='.'))return 0;
    return 1;
}
static int revision(const char *text,uint64_t *out) {
    if(!text||!*text||*text=='0')return -1;
    uint64_t value=0;
    for(;*text;text++){
        if(*text<'0'||*text>'9'||value>(UINT64_MAX-(unsigned)(*text-'0'))/10)return -1;
        value=value*10+(unsigned)(*text-'0');
    }
    *out=value;return 0;
}
static int execute(CnetCapsuleStore *store,char *line) {
    char *arg[5]={0},*save=NULL;unsigned n=0;
    for(const char *p=line;*p;p++)if((unsigned char)*p<32||(unsigned char)*p>126)return -1;
    for(char *p=strtok_r(line," ",&save);p;p=strtok_r(NULL," ",&save)) {
        if(n==5)return -1;
        arg[n++]=p;
    }
    if(!n)return -1;
    if(n==1&&!strcmp(arg[0],"STATUS"))return 0;
    uint64_t expected;
    if(n<2||revision(arg[1],&expected))return -1;
    if(n==4&&!strcmp(arg[0],"STAGE")&&(!strcmp(arg[2],"0")||!strcmp(arg[2],"1"))) {
        char hash[65];return cnet_capsule_store_stage(store,expected,arg[3],arg[2][0]-'0',hash);
    }
    if(n==4&&!strcmp(arg[0],"ACTIVATE"))return cnet_capsule_store_activate(store,expected,arg[2],arg[3]);
    if(n==3&&!strcmp(arg[0],"UNLOAD"))return cnet_capsule_store_unload(store,expected,arg[2]);
    if(n==3&&!strcmp(arg[0],"ROLLBACK"))return cnet_capsule_store_rollback(store,expected,arg[2]);
    if(n==3&&!strcmp(arg[0],"DISCARD")) {
        CnetCapsuleStoreStatus st;
        if(cnet_capsule_store_status(store,&st)||st.revision!=expected)return -1;
        return cnet_capsule_store_discard(store,arg[2]);
    }
    return -1;
}
CnetCapsuleControl *cnet_capsule_control_open(CnetCapsuleStore *store,const char *path) {
    if(!store||!path||*path!='/'||strlen(path)>=sizeof(((struct sockaddr_un*)0)->sun_path))return NULL;
    char parent[108];snprintf(parent,sizeof parent,"%s",path);
    char *slash=strrchr(parent,'/');if(!slash||!name_ok(slash+1))return NULL;
    CnetCapsuleControl *c=calloc(1,sizeof *c);if(!c)return NULL;
    c->fd=c->parent=c->lock=-1;c->store=store;snprintf(c->name,sizeof c->name,"%s",slash+1);*slash=0;
    c->parent=cnet_capsule_owner_directory(parent,1);if(c->parent<0)goto fail;
    char lockname[100];snprintf(lockname,sizeof lockname,"%.80s.lock",c->name);
    c->lock=openat(c->parent,lockname,O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    struct stat st;
    if(c->lock<0||fstat(c->lock,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||
       (st.st_mode&077)||st.st_nlink!=1||st.st_size||flock(c->lock,LOCK_EX|LOCK_NB))goto fail;
    struct sockaddr_un address={0};address.sun_family=AF_UNIX;
    snprintf(address.sun_path,sizeof address.sun_path,"/proc/self/fd/%d/%.80s",c->parent,c->name);
    if(!fstatat(c->parent,c->name,&st,AT_SYMLINK_NOFOLLOW)) {
        if(!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077))goto fail;
        int probe=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(probe<0)goto fail;
        int active=connect(probe,(struct sockaddr*)&address,sizeof address);int error=errno;close(probe);
        if(!active||error!=ECONNREFUSED)goto fail;
        struct stat current;
        if(fstatat(c->parent,c->name,&current,AT_SYMLINK_NOFOLLOW)||current.st_dev!=st.st_dev||
           current.st_ino!=st.st_ino||unlinkat(c->parent,c->name,0))goto fail;
    } else if(errno!=ENOENT)goto fail;
    c->fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(c->fd<0)goto fail;
    /* Private parent prevents exposure during bind->chmod. */
    if(bind(c->fd,(struct sockaddr*)&address,sizeof address))goto fail;
    if(fstatat(c->parent,c->name,&st,AT_SYMLINK_NOFOLLOW))goto fail;
    c->bound=1;c->device=st.st_dev;c->inode=st.st_ino;
    if(fchmodat(c->parent,c->name,0600,0)||listen(c->fd,8))goto fail;
    return c;
fail:cnet_capsule_control_close(c);return NULL;
}
int cnet_capsule_control_fd(const CnetCapsuleControl *c){return c?c->fd:-1;}
void cnet_capsule_control_accept(CnetCapsuleControl *c) {
    if(!c)return;
    int fd=accept4(c->fd,NULL,NULL,SOCK_CLOEXEC);if(fd<0)return;
    struct ucred peer;socklen_t size=sizeof peer;char line[512],reply[400];
    int rc=-1;
    if(!getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&size)&&size==sizeof peer&&peer.uid==geteuid()&&
       cnetd_read_request(fd,line,sizeof line,5000)==CNETD_READ_OK)rc=execute(c->store,line);
    CnetCapsuleStoreStatus st={0};(void)cnet_capsule_store_status(c->store,&st);
    int n=snprintf(reply,sizeof reply,"%s revision=%llu active=%s rollback=%s staged=%s durable=%d reason=%s\n",
        rc?"ERR":"OK",(unsigned long long)st.revision,
        st.active[0]?st.active:"-",st.rollback[0]?st.rollback:"-",st.staged[0]?st.staged:"-",!st.durability_uncertain,
        rc==-2?"durability_uncertain":rc?"refused":"ok");
    if(n>0&&(size_t)n<sizeof reply)(void)send(fd,reply,(size_t)n,MSG_NOSIGNAL|MSG_DONTWAIT);
    close(fd);
}
void cnet_capsule_control_close(CnetCapsuleControl *c) {
    if(!c)return;
    if(c->fd>=0)close(c->fd);
    if(c->bound){struct stat st;
        if(!fstatat(c->parent,c->name,&st,AT_SYMLINK_NOFOLLOW)&&st.st_dev==c->device&&st.st_ino==c->inode)
            (void)unlinkat(c->parent,c->name,0);
    }
    if(c->lock>=0)close(c->lock);
    if(c->parent>=0)close(c->parent);
    free(c);
}
