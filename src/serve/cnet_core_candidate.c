#define _GNU_SOURCE
#include "cnet_core_candidate.h"
#include "cce/cce_campaign_provenance.h"
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
_Static_assert(sizeof(float)==4&&FLT_RADIX==2&&FLT_MANT_DIG==24&&FLT_MAX_EXP==128,"requires IEEE binary32");
static void put32(unsigned char *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
static uint32_t get32(const unsigned char *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v|=(uint32_t)p[i]<<(8*i);return v;}
static int digest(const char *s){
    int nonzero=0;for(unsigned i=0;i<64;i++){if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return -1;nonzero|=s[i]!='0';}
    return s[64]||!nonzero?-1:0;
}
static int boundary(int dir,const char *name){
    if(!name||!name[0]||name[0]=='.'||strnlen(name,97)>96)return -1;
    for(unsigned i=0;name[i];i++)if(!((name[i]>='a'&&name[i]<='z')||(name[i]>='A'&&name[i]<='Z')||
        (name[i]>='0'&&name[i]<='9')||name[i]=='_'||name[i]=='-'||name[i]=='.'))return -1;
    struct stat s;return fstat(dir,&s)||!S_ISDIR(s.st_mode)||s.st_uid!=geteuid()||(s.st_mode&077)?-1:0;
}
static int transfer(int fd,void *buffer,size_t bytes,int writing){
    unsigned char *p=buffer;while(bytes){ssize_t n=writing?write(fd,p,bytes):read(fd,p,bytes);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;bytes-=(size_t)n;}return 0;
}
static void header(unsigned char *b,unsigned objective){
    static const uint32_t values[]={1,1,1,1,3,8,1,41,0,0};
    memcpy(b,"CNETCEL1",8);for(unsigned i=0;i<10;i++)put32(b+8+i*4,values[i]);
    put32(b+12,objective);
}
int cnet_core_candidate_save_objective_at(int dir,const char *name,const CnetCoreCandidate *c,unsigned objective){
    if((objective!=CNET_CORE_OBJECTIVE_GRAPH&&objective!=CNET_CORE_OBJECTIVE_ALLOCATOR)||boundary(dir,name)||!c||cnet_core_cell_validate(&c->cell)||digest(c->training_sha256)||digest(c->evaluation_sha256))return -1;
    unsigned char b[CNET_CORE_CANDIDATE_BYTES]={0};header(b,objective);memcpy(b+48,c->training_sha256,64);memcpy(b+112,c->evaluation_sha256,64);
    for(unsigned i=0;i<41;i++){uint32_t v;memcpy(&v,c->cell.weight+i,4);put32(b+176+i*4,v);}
    char hash[65];if(cce_sha256_bytes_hex(b,340,hash))return -1;memcpy(b+340,hash,64);
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return -1;
    int rc=transfer(fd,b,sizeof b,1)||fsync(fd);if(close(fd))rc=-1;
    if(!rc&&fsync(dir))rc=-1;
    /* Preserve a created file on error for diagnosis; never replace/unlink a
     * path that another owner operation could have exchanged concurrently. */
    return rc?-1:0;
}
int cnet_core_candidate_load_objective_at(int dir,const char *name,CnetCoreCandidate *out,unsigned objective){
    if(!out)return -1;
    memset(out,0,sizeof *out);if((objective!=CNET_CORE_OBJECTIVE_GRAPH&&objective!=CNET_CORE_OBJECTIVE_ALLOCATOR)||boundary(dir,name))return -1;
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);if(fd<0)return -1;
    struct stat s;unsigned char b[CNET_CORE_CANDIDATE_BYTES],expected[48];
    int rc=fstat(fd,&s)||!S_ISREG(s.st_mode)||s.st_uid!=geteuid()||(s.st_mode&077)||s.st_nlink!=1||s.st_size!=sizeof b;
    if(!rc){rc=transfer(fd,b,sizeof b,0);unsigned char extra;if(!rc&&read(fd,&extra,1)!=0)rc=-1;}
    if(close(fd))rc=-1;
    if(rc)return -1;
    header(expected,objective);char hash[65];if(memcmp(b,expected,48)||cce_sha256_bytes_hex(b,340,hash)||memcmp(b+340,hash,64))return -1;
    CnetCoreCandidate c={0};memcpy(c.training_sha256,b+48,64);memcpy(c.evaluation_sha256,b+112,64);
    for(unsigned i=0;i<41;i++){uint32_t v=get32(b+176+i*4);memcpy(c.cell.weight+i,&v,4);}
    if(digest(c.training_sha256)||digest(c.evaluation_sha256)||cnet_core_cell_validate(&c.cell))return -1;
    *out=c;return 0;
}
int cnet_core_candidate_save_at(int dir,const char *name,const CnetCoreCandidate *c){return cnet_core_candidate_save_objective_at(dir,name,c,CNET_CORE_OBJECTIVE_GRAPH);}
int cnet_core_candidate_load_at(int dir,const char *name,CnetCoreCandidate *c){return cnet_core_candidate_load_objective_at(dir,name,c,CNET_CORE_OBJECTIVE_GRAPH);}
