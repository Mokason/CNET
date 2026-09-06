#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cnet_capsule_snapshot.h"
#include "cce/cce_campaign_provenance.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SNAP_UNITS 4096
#define SNAP_BYTES (UINT64_C(32)*1024*1024*1024)
typedef struct {char name[97];} Entry;
static const char *const artifacts[]={"manifest.cknow","unit.cnb","frontend.cvfa"};
static int component(const char *s) {
    if(!s||!s[0]||s[0]=='.'||strlen(s)>96)return 0;
    for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||
        (*s>='0'&&*s<='9')||*s=='_'||*s=='-'||*s=='.'))return 0;
    return 1;
}
static int hash_name(const char *s) {
    if(!s||strlen(s)!=64)return 0;
    for(unsigned i=0;i<64;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;
    return 1;
}
static int directory(int fd,int private_mode,struct stat *s) {
    return fstat(fd,s)||!S_ISDIR(s->st_mode)||s->st_uid!=geteuid()||
           (s->st_mode&(private_mode?077:022))?-1:0;
}
/* Ancestors are pinned before descending. A shared sticky parent (e.g. /tmp)
 * is allowed; the selected final directory itself must be owner controlled. */
int cnet_capsule_owner_directory(const char *path,int private_mode) {
    if(!path||!*path||strlen(path)>4095)return -1;
    char buffer[4096];snprintf(buffer,sizeof buffer,"%s",path);
    int fd=open(path[0]=='/'?"/":".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)return -1;
    char *save=NULL;
    for(char *part=strtok_r(buffer,"/",&save);part;part=strtok_r(NULL,"/",&save)) {
        struct stat parent;
        if(!strcmp(part,".")||!strcmp(part,"..")||fstat(fd,&parent)||
           (parent.st_uid!=0&&parent.st_uid!=geteuid())||
           ((parent.st_mode&022)&&!(parent.st_mode&S_ISVTX))) {close(fd);return -1;}
        int next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        close(fd);if(next<0)return -1;fd=next;
    }
    struct stat s;if(directory(fd,private_mode,&s)){close(fd);return -1;}return fd;
}
static int unchanged(const struct stat *a,const struct stat *b) {
    return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_size==b->st_size&&
        a->st_mode==b->st_mode&&a->st_uid==b->st_uid&&a->st_nlink==b->st_nlink&&
        a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&
        a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
static int order(const void *a,const void *b){return strcmp(((const Entry*)a)->name,((const Entry*)b)->name);}
static int entries_filtered(int fd,Entry *list,size_t cap,size_t *count,int publishing) {
    int scan=openat(fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(scan<0)return -1;
    DIR *d=fdopendir(scan);if(!d){close(scan);return -1;}
    size_t n=0;int rc=0;struct dirent *e;errno=0;
    while((e=readdir(d))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        if(publishing&&!strcmp(e->d_name,".publish.lock"))continue;
        if(n==cap||!component(e->d_name)){rc=-1;break;}
        snprintf(list[n++].name,sizeof list[0].name,"%s",e->d_name);
    }
    if(errno)rc=-1;
    if(closedir(d))rc=-1;
    if(rc)return -1;
    qsort(list,n,sizeof *list,order);*count=n;return 0;
}
static int entries(int fd,Entry *list,size_t cap,size_t *count) {
    return entries_filtered(fd,list,cap,count,0);
}
static int transfer(int fd,void *data,size_t length,int writing) {
    unsigned char *p=data;
    while(length){ssize_t n=writing?write(fd,p,length):read(fd,p,length);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return -1;
        p+=n;length-=(size_t)n;
    }
    return 0;
}
static int artifact(int source,int target,const char *name,char hash[65],size_t *total) {
    int fd=openat(source,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0)return -1;
    struct stat before,after;int rc=-1;
    size_t limit=!strcmp(name,"unit.cnb")?64UL*1024*1024:16UL*1024*1024;
    if(fstat(fd,&before)||!S_ISREG(before.st_mode)||before.st_uid!=geteuid()||
       (before.st_mode&022)||before.st_nlink!=1||before.st_size<1||
       (uint64_t)before.st_size>limit||(uint64_t)*total+(uint64_t)before.st_size>SNAP_BYTES)goto done;
    size_t length=(size_t)before.st_size;unsigned char *data=malloc(length);
    if(!data)goto done;
    unsigned char extra;
    if(transfer(fd,data,length,0)||read(fd,&extra,1)!=0||fstat(fd,&after)||!unchanged(&before,&after)||
       cce_sha256_bytes_hex(data,length,hash))goto release;
    if(target>=0){
        int out=openat(target,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0400);
        if(out<0)goto release;
        int failed=transfer(out,data,length,1)||fsync(out);if(close(out))failed=1;
        if(failed)goto release;
    }
    *total+=length;rc=0;
release:free(data);
done:if(close(fd))rc=-1;return rc;
}
/* Digest a canonical sorted inventory, not directory order or mutable paths. */
static int inventory(int source,int target,char digest[65],size_t *bytes,int publishing) {
    Entry *units=calloc(SNAP_UNITS,sizeof *units);size_t count=0,total=0;
    size_t cap=32+SNAP_UNITS*384;char *canonical=calloc(1,cap);int rc=-1;
    struct stat before,after;
    if(!units||!canonical||directory(source,0,&before)||entries_filtered(source,units,SNAP_UNITS,&count,publishing))goto done;
    size_t used=(size_t)snprintf(canonical,cap,"CNET-SNAPSHOT-1\n%zu\n",count);
    for(size_t i=0;i<count;i++) {
        int src=openat(source,units[i].name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC),dst=-1;
        Entry files[3];size_t n=0;struct stat a,b;
        if(src<0)goto done;
        int failed=directory(src,0,&a)||entries(src,files,3,&n)||n<2;
        unsigned mask=0;
        for(size_t j=0;j<n&&!failed;j++) {
            unsigned k=0;while(k<3&&strcmp(files[j].name,artifacts[k]))k++;
            if(k==3)failed=1;else mask|=1u<<k;
        }
        if((mask&3)!=3)failed=1;
        if(!failed&&target>=0) {
            if(mkdirat(target,units[i].name,0700))failed=1;
            else if((dst=openat(target,units[i].name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC))<0)failed=1;
        }
        used+=(size_t)snprintf(canonical+used,cap-used,"%s\n",units[i].name);
        for(unsigned k=0;k<3&&!failed;k++)if(mask&(1u<<k)){
            char hash[65];if(artifact(src,dst,artifacts[k],hash,&total))failed=1;
            else used+=(size_t)snprintf(canonical+used,cap-used,"%u %s\n",k,hash);
        }
        if(!failed&&(fstat(src,&b)||!unchanged(&a,&b)))failed=1;
        if(dst>=0){if(fsync(dst)||fchmod(dst,0500)||fsync(dst))failed=1;if(close(dst))failed=1;}
        if(close(src))failed=1;
        if(failed||used>=cap)goto done;
    }
    if(fstat(source,&after)||!unchanged(&before,&after)||cce_sha256_bytes_hex(canonical,used,digest))goto done;
    *bytes=total;rc=0;
done:free(canonical);free(units);return rc;
}
int cnet_capsule_snapshot_verify(int directory_fd,const char *digest,size_t *bytes) {
    struct stat st;if(!bytes||!hash_name(digest)||directory(directory_fd,1,&st))return -1;
    int fd=openat(directory_fd,digest,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    char actual[65];int rc=inventory(fd,-1,actual,bytes,0);
    if(close(fd))rc=-1;
    return rc||strcmp(actual,digest)?-1:0;
}
/* Only private temporary directories created by this call are removed. Never
 * follow names from source content, nor remove an existing digest directory. */
static void remove_temporary(int parent,const char *name,int fd) {
    Entry *units=calloc(SNAP_UNITS,sizeof *units);size_t count=0;
    if(units&&!entries(fd,units,SNAP_UNITS,&count))for(size_t i=0;i<count;i++) {
        int child=openat(fd,units[i].name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(child<0)continue;
        if(!fchmod(child,0700)){for(unsigned k=0;k<3;k++)(void)unlinkat(child,artifacts[k],0);}
        close(child);(void)unlinkat(fd,units[i].name,AT_REMOVEDIR);
    }
    free(units);(void)unlinkat(parent,name,AT_REMOVEDIR);
}
int cnet_capsule_snapshot_create(int source,int destination,const char *name,char digest[65],size_t *bytes) {
    if(digest)digest[0]=0;
    if(bytes)*bytes=0;
    struct stat st;if(!digest||!bytes||directory(source,0,&st)||directory(destination,1,&st)||
        (name&&!component(name)))return -1;
    int src=name?openat(source,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):dup(source);
    if(src<0)return -1;
    /* Cooperate with the existing native teach publisher. Only its exact
     * private zero-length lock is metadata, never silently ignore dotfiles. */
    int publisher=openat(src,".publish.lock",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(publisher>=0) {
        if(fstat(publisher,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||
           (st.st_mode&077)||st.st_nlink!=1||st.st_size||flock(publisher,LOCK_SH|LOCK_NB)) {
            close(publisher);close(src);return -1;
        }
    } else if(errno!=ENOENT){close(src);return -1;}
    unsigned char nonce[16];char temporary[42]="pending-";
    if(getrandom(nonce,sizeof nonce,0)!=sizeof nonce){if(publisher>=0)close(publisher);close(src);return -1;}
    for(unsigned i=0;i<16;i++)snprintf(temporary+8+i*2,3,"%02x",nonce[i]);
    if(mkdirat(destination,temporary,0700)){if(publisher>=0)close(publisher);close(src);return -1;}
    int dst=openat(destination,temporary,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int rc=dst<0?-1:inventory(src,dst,digest,bytes,publisher>=0);if(close(src))rc=-1;
    if(publisher>=0)close(publisher);
    if(!rc&&(fsync(dst)||fchmod(dst,0500)||fsync(dst)))rc=-1;
    int published=0;
    if(!rc) {
        if(!syscall(SYS_renameat2,destination,temporary,destination,digest,1))published=1;
        else if(errno!=EEXIST)rc=-1;
        else {size_t existing=0;rc=cnet_capsule_snapshot_verify(destination,digest,&existing);if(!rc&&existing!=*bytes)rc=-1;}
    }
    if(dst>=0){if(!published){(void)fchmod(dst,0700);remove_temporary(destination,temporary,dst);}close(dst);}
    if(!rc&&fsync(destination))rc=-1;
    if(rc)digest[0]=0;
    return rc;
}
static int pending_name(const char *s) {
    if(strncmp(s,"pending-",8)||strlen(s)!=40)return 0;
    for(unsigned i=8;i<40;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;
    return 1;
}
/* Validate the complete removal target before unlinking any artifact. Partial
 * writes may be zero bytes, but no arbitrary file names, links or owners pass. */
static int removable(int fd) {
    struct stat st;Entry *units=calloc(SNAP_UNITS,sizeof *units);size_t count=0;int rc=-1;
    if(!units||directory(fd,1,&st)||entries(fd,units,SNAP_UNITS,&count))goto done;
    for(size_t i=0;i<count;i++) {
        int child=openat(fd,units[i].name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(child<0)goto done;
        Entry files[3];size_t n=0;int bad=directory(child,1,&st)||entries(child,files,3,&n);
        for(size_t j=0;j<n&&!bad;j++) {
            unsigned k=0;while(k<3&&strcmp(files[j].name,artifacts[k]))k++;
            if(k==3||fstatat(child,files[j].name,&st,AT_SYMLINK_NOFOLLOW)||
               !S_ISREG(st.st_mode)||st.st_uid!=geteuid()||st.st_nlink!=1||(st.st_mode&077))bad=1;
        }
        close(child);if(bad)goto done;
    }
    rc=0;
done:free(units);return rc;
}
int cnet_capsule_snapshot_collect(int parent,const char *active,const char *rollback,const char *staged) {
    struct stat st;Entry *list=calloc(SNAP_UNITS,sizeof *list);size_t count=0;int rc=-1;
    if(!active||!rollback||!staged||!list||directory(parent,1,&st)||entries(parent,list,SNAP_UNITS,&count))goto done;
    for(size_t i=0;i<count;i++) {
        const char *name=list[i].name;
        if(!strcmp(name,active)||!strcmp(name,rollback)||!strcmp(name,staged))continue;
        if(!hash_name(name)&&!pending_name(name))goto done;
        int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0)goto done;
        if(removable(fd)||fchmod(fd,0700)){close(fd);goto done;}
        remove_temporary(parent,name,fd);close(fd);
        if(!fstatat(parent,name,&st,AT_SYMLINK_NOFOLLOW)||errno!=ENOENT)goto done;
    }
    rc=fsync(parent)?-1:0;
done:free(list);return rc;
}
