#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cnet_capsule_store.h"
#include "cnet_capsule_snapshot.h"
#include "cce/cce_campaign_provenance.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#define HISTORY_MAX 16384
#define RECORD_HEADER 256
#define RECORD_ROW 129
struct CnetCapsuleStore {
    int sets,state,snapshots,lock,uncertain;
    CnetCoreHost *host;
    uint64_t revision,staged_id;
    char active[65],rollback[65],staged[65],last[65],prepared_key[65];
};
static int identifier(const char *s) {
    if(!s||!*s||strlen(s)>63)return 0;
    for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||
       (*s>='0'&&*s<='9')||*s=='_'||*s=='-'))return 0;
    return 1;
}
static int digest(const char *s) {
    if(!s||strlen(s)!=64)return 0;
    for(unsigned i=0;i<64;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;
    return 1;
}
static int io(int fd,void *data,size_t n,int writing) {
    unsigned char *p=data;
    while(n){ssize_t got=writing?write(fd,p,n):read(fd,p,n);
        if(got<0&&errno==EINTR)continue;
        if(got<=0)return -1;
        p+=got;n-=(size_t)got;
    }
    return 0;
}
static int owned_file(int fd,size_t maximum,struct stat *st) {
    return fstat(fd,st)||!S_ISREG(st->st_mode)||st->st_uid!=geteuid()||
        (st->st_mode&077)||st->st_nlink!=1||st->st_size<0||(uint64_t)st->st_size>maximum?-1:0;
}
static void put64(unsigned char *b,uint64_t n){for(unsigned i=0;i<8;i++)b[i]=(unsigned char)(n>>(i*8));}
static uint64_t get64(const unsigned char *b){uint64_t n=0;for(unsigned i=0;i<8;i++)n|=(uint64_t)b[i]<<(i*8);return n;}
/* Explicit byte encoding. No compiler structs, pointers, or host padding persist. */
static unsigned char *record(CnetCapsuleStore *s,uint64_t revision,const char *active,
        const char *rollback,const char *last,uint64_t staged,size_t *length) {
    CnetCapsuleIdentity *known=calloc(HISTORY_MAX,sizeof *known);size_t count=0;
    if(!known||cnet_core_host_history(s->host,staged,known,HISTORY_MAX,&count)){free(known);return NULL;}
    *length=RECORD_HEADER+count*RECORD_ROW+64;
    unsigned char *b=calloc(1,*length);if(!b){free(known);return NULL;}
    memcpy(b,"CNET-ACTIVE-1",13);put64(b+16,revision);
    memcpy(b+24,active,65);memcpy(b+89,rollback,65);memcpy(b+154,last,65);put64(b+224,count);
    for(size_t i=0;i<count;i++){
        memcpy(b+RECORD_HEADER+i*RECORD_ROW,known[i].unit,64);
        memcpy(b+RECORD_HEADER+i*RECORD_ROW+64,known[i].sha256,65);
    }
    char hash[65];int failed=cce_sha256_bytes_hex(b,*length-64,hash);free(known);
    if(failed){free(b);return NULL;}memcpy(b+*length-64,hash,64);return b;
}
static int sync_point(int fd,const char *point) {
#ifdef CNET_CAPSULE_STORE_TESTING
    const char *crash=getenv("CNET_CAPSULE_STORE_CRASH_SYNC");
    if(crash&&!strcmp(crash,point))_exit(86);
    const char *fail=getenv("CNET_CAPSULE_STORE_FAIL_SYNC");
    if(fail&&!strcmp(fail,point)){errno=EIO;return -1;}
#else
    (void)point;
#endif
    return fsync(fd);
}
/* rename is the visibility commit point; a later error is never rollback. */
static int publish(CnetCapsuleStore *s,const unsigned char *b,size_t length) {
    unsigned char nonce[16];char name[64]="selection-pending-";
    if(getrandom(nonce,sizeof nonce,0)!=sizeof nonce)return -1;
    for(unsigned i=0;i<16;i++)snprintf(name+18+i*2,3,"%02x",nonce[i]);
    int fd=openat(s->state,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return -1;
    int failed=io(fd,(void *)b,length,1)||sync_point(fd,"record");if(close(fd))failed=1;
    if(failed){unlinkat(s->state,name,0);return -1;}
    if(sync_point(s->state,"before_rename")||renameat(s->state,name,s->state,"selection")) {
        unlinkat(s->state,name,0);return -1;
    }
    if(sync_point(s->state,"after_rename")){s->uncertain=1;return -2;}
    return 0;
}
static void snapshot_path(CnetCapsuleStore *s,const char *hash,char path[128]) {
    snprintf(path,128,"/proc/self/fd/%d/%s",s->snapshots,hash);
}
static int empty_snapshot(CnetCapsuleStore *s,char hash[65]) {
    if(mkdirat(s->state,"empty",0700)&&errno!=EEXIST)return -1;
    int fd=openat(s->state,"empty",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    size_t bytes=0;int rc=cnet_capsule_snapshot_create(fd,s->snapshots,NULL,hash,&bytes);
    if(close(fd))rc=-1;
    return rc||bytes?-1:0;
}
static int load(CnetCapsuleStore *s) {
    int fd=openat(s->state,"selection",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0)return errno==ENOENT?1:-1;
    struct stat st;unsigned char *b=NULL;CnetCapsuleIdentity *known=NULL;int rc=-1;
    if(owned_file(fd,RECORD_HEADER+HISTORY_MAX*RECORD_ROW+64,&st)||st.st_size<RECORD_HEADER+64)goto done;
    size_t length=(size_t)st.st_size;b=malloc(length);if(!b||io(fd,b,length,0))goto done;
    unsigned char extra;if(read(fd,&extra,1)!=0)goto done;
    char hash[65];if(cce_sha256_bytes_hex(b,length-64,hash)||memcmp(hash,b+length-64,64))goto done;
    unsigned char magic[16]={0};memcpy(magic,"CNET-ACTIVE-1",13);
    if(memcmp(b,magic,16)||!get64(b+16)||b[88]||b[153]||b[218])goto done;
    for(unsigned i=219;i<256;i++)if((i<224||i>=232)&&b[i])goto done;
    if(!digest((char *)b+24)||(b[89]&&!digest((char *)b+89))||(b[154]&&!digest((char *)b+154)))goto done;
    if(!b[89])for(unsigned i=89;i<154;i++)if(b[i])goto done;
    if(!b[154])for(unsigned i=154;i<219;i++)if(b[i])goto done;
    uint64_t count=get64(b+224);
    if(count>HISTORY_MAX||length!=RECORD_HEADER+count*RECORD_ROW+64)goto done;
    known=calloc((size_t)count+1,sizeof *known);if(!known)goto done;
    for(size_t i=0;i<(size_t)count;i++){
        memcpy(known[i].unit,b+RECORD_HEADER+i*RECORD_ROW,64);
        memcpy(known[i].sha256,b+RECORD_HEADER+i*RECORD_ROW+64,65);
        if(!memchr(known[i].unit,0,64)||known[i].sha256[64])goto done;
        size_t n=strlen(known[i].unit);for(size_t j=n;j<64;j++)if(known[i].unit[j])goto done;
    }
    size_t bytes;char path[128];
    if(cnet_capsule_snapshot_verify(s->snapshots,(char *)b+24,&bytes)||
        (b[89]&&cnet_capsule_snapshot_verify(s->snapshots,(char *)b+89,&bytes)))goto done;
    snapshot_path(s,(char *)b+24,path);s->host=cnet_core_host_open(path);
    if(!s->host||cnet_core_host_restore_history(s->host,known,(size_t)count))goto done;
    /* Both references must be semantically valid and consistent with history,
       not merely hash-valid. Rollback must be usable after recovery. */
    if(b[89]) {
        uint64_t staged;snapshot_path(s,(char *)b+89,path);
        if(cnet_core_host_stage_registry(s->host,path,0,&staged)||cnet_core_host_discard(s->host,staged))goto done;
    }
    if(fsync(fd)||fsync(s->state))goto done;
    s->revision=get64(b+16);memcpy(s->active,b+24,65);memcpy(s->rollback,b+89,65);memcpy(s->last,b+154,65);rc=0;
done:free(known);free(b);if(close(fd))rc=-1;return rc;
}
static int collect_pending_records(CnetCapsuleStore *s) {
    int scan=openat(s->state,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(scan<0)return -1;
    DIR *d=fdopendir(scan);if(!d){close(scan);return -1;}
    unsigned count=0;int rc=0;struct dirent *e;
    for(;;) {
        errno=0;e=readdir(d);if(!e){if(errno)rc=-1;break;}
        if(++count>4096){rc=-1;break;}
        if(strncmp(e->d_name,"selection-pending-",18))continue;
        if(strlen(e->d_name)!=50){rc=-1;break;}
        for(unsigned i=18;i<50;i++)if(!((e->d_name[i]>='0'&&e->d_name[i]<='9')||
            (e->d_name[i]>='a'&&e->d_name[i]<='f')))rc=-1;
        if(rc)break;
        int fd=openat(s->state,e->d_name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        struct stat st;
        if(fd<0){rc=-1;break;}
        rc=owned_file(fd,RECORD_HEADER+HISTORY_MAX*RECORD_ROW+64,&st);
        if(close(fd))rc=-1;
        if(rc||unlinkat(s->state,e->d_name,0)){rc=-1;break;}
    }
    if(closedir(d)||fsync(s->state))rc=-1;
    return rc;
}
CnetCapsuleStore *cnet_capsule_store_open(const char *sets,const char *state) {
    if(!sets||!state||sets[0]!='/'||state[0]!='/')return NULL;
    CnetCapsuleStore *s=calloc(1,sizeof *s);if(!s)return NULL;
    s->sets=s->state=s->snapshots=s->lock=-1;
    s->sets=cnet_capsule_owner_directory(sets,1);s->state=cnet_capsule_owner_directory(state,1);
    if(s->sets<0||s->state<0)goto fail;
    struct stat st;
    s->lock=openat(s->state,"owner.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    if(s->lock<0||owned_file(s->lock,0,&st)||flock(s->lock,LOCK_EX|LOCK_NB))goto fail;
    if(mkdirat(s->state,"snapshots",0700)&&errno!=EEXIST)goto fail;
    s->snapshots=openat(s->state,"snapshots",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(s->snapshots<0||fsync(s->state))goto fail;
    int rc=load(s);if(rc<0)goto fail;
    if(rc==1) {
        char path[128];size_t length;
        if(empty_snapshot(s,s->active))goto fail;
        snapshot_path(s,s->active,path);s->host=cnet_core_host_open(path);if(!s->host)goto fail;
        s->revision=1;unsigned char *b=record(s,1,s->active,s->rollback,s->last,0,&length);
        if(!b)goto fail;
        rc=publish(s,b,length);free(b);if(rc)goto fail;
    }
    if(collect_pending_records(s)||cnet_capsule_snapshot_collect(s->snapshots,s->active,s->rollback,s->staged))goto fail;
    return s;
fail:(void)cnet_capsule_store_close(s);return NULL;
}
int cnet_capsule_store_close(CnetCapsuleStore *s) {
    if(!s)return -1;
    if(s->host&&cnet_core_host_close(s->host))return -1;
    if(s->snapshots>=0)close(s->snapshots);
    if(s->lock>=0)close(s->lock);
    if(s->state>=0)close(s->state);
    if(s->sets>=0)close(s->sets);
    free(s);return 0;
}
int cnet_capsule_store_status(CnetCapsuleStore *s,CnetCapsuleStoreStatus *out) {
    if(!s||!out)return -1;
    memset(out,0,sizeof *out);out->revision=s->revision;out->durability_uncertain=s->uncertain;
    memcpy(out->active,s->active,65);memcpy(out->rollback,s->rollback,65);memcpy(out->staged,s->staged,65);return 0;
}
CnetCoreLease *cnet_capsule_store_pin(CnetCapsuleStore *s){return s?cnet_core_host_pin(s->host):NULL;}
int cnet_capsule_store_stage(CnetCapsuleStore *s,uint64_t expected,const char *name,int preserve,char hash[65]) {
    if(hash)hash[0]=0;
    if(!s||!hash||!identifier(name)||s->uncertain||expected!=s->revision||s->staged_id||
        (preserve!=0&&preserve!=1))return -1;
    if(cnet_capsule_snapshot_collect(s->snapshots,s->active,s->rollback,s->staged))return -1;
    size_t bytes;char path[128];
    if(cnet_capsule_snapshot_create(s->sets,s->snapshots,name,hash,&bytes))return -1;
    snapshot_path(s,hash,path);
    if(cnet_core_host_stage_registry(s->host,path,preserve,&s->staged_id)){hash[0]=0;return -1;}
    memcpy(s->staged,hash,65);return 0;
}
int cnet_capsule_store_discard(CnetCapsuleStore *s,const char *hash) {
    if(!s||s->uncertain||!digest(hash)||strcmp(hash,s->staged)||!s->staged_id||
        cnet_core_host_discard(s->host,s->staged_id))return -1;
    s->staged_id=0;s->staged[0]=0;s->prepared_key[0]=0;return 0;
}
static int request(CnetCapsuleStore *s,uint64_t expected,const char *token,const char *op,
                   const char *target,char key[65]) {
    if(!s||s->uncertain||!identifier(token)||expected==UINT64_MAX)return -1;
    char message[256];int n=snprintf(message,sizeof message,"%s\n%llu\n%s\n%s\n",op,
        (unsigned long long)expected,token,target);
    if(n<0||(size_t)n>=sizeof message||cce_sha256_bytes_hex(message,(size_t)n,key))return -1;
    if(expected+1==s->revision&&!strcmp(s->last,key))return 1;
    return expected==s->revision?0:-1;
}
static int commit(CnetCapsuleStore *s,const char *key) {
    size_t bytes;
    if(cnet_capsule_snapshot_verify(s->snapshots,s->staged,&bytes)||
       cnet_capsule_snapshot_verify(s->snapshots,s->active,&bytes))return -1;
    size_t length;unsigned char *b=record(s,s->revision+1,s->staged,s->active,key,s->staged_id,&length);
    if(!b)return -1;
    int rc=publish(s,b,length);free(b);if(rc)return rc;
    /* Serial store ownership keeps the checked stage valid across publication. */
    if(cnet_core_host_activate(s->host,s->staged_id)){s->uncertain=1;return -2;}
    memcpy(s->rollback,s->active,65);memcpy(s->active,s->staged,65);memcpy(s->last,key,65);
    s->revision++;s->staged_id=0;s->staged[0]=0;s->prepared_key[0]=0;return 0;
}
int cnet_capsule_store_activate(CnetCapsuleStore *s,uint64_t expected,const char *token,const char *hash) {
    if(!digest(hash))return -1;
    char key[65];int retry=request(s,expected,token,"activate",hash,key);
    if(retry)return retry==1?0:-1;
    if(!s->staged_id||strcmp(hash,s->staged))return -1;
    return commit(s,key);
}
static int select_existing(CnetCapsuleStore *s,uint64_t expected,const char *token,int rollback) {
    char key[65];int retry=request(s,expected,token,rollback?"rollback":"unload","",key);
    if(retry)return retry==1?0:-1;
    if(s->staged_id)return !strcmp(s->prepared_key,key)?commit(s,key):-1;
    char hash[65]={0},path[128];size_t bytes;
    if(rollback) {
        if(!s->rollback[0]||cnet_capsule_snapshot_verify(s->snapshots,s->rollback,&bytes))return -1;
        memcpy(hash,s->rollback,65);
    } else if(empty_snapshot(s,hash))return -1;
    snapshot_path(s,hash,path);
    if(cnet_core_host_stage_registry(s->host,path,0,&s->staged_id))return -1;
    memcpy(s->staged,hash,65);memcpy(s->prepared_key,key,65);return commit(s,key);
}
int cnet_capsule_store_unload(CnetCapsuleStore *s,uint64_t expected,const char *token){return select_existing(s,expected,token,0);}
int cnet_capsule_store_rollback(CnetCapsuleStore *s,uint64_t expected,const char *token){return select_existing(s,expected,token,1);}
