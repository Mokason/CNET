#define _GNU_SOURCE
#include "allocator_data.h"
#include "cce/cce_campaign_provenance.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int integer(const char *s,uint64_t max,unsigned base,uint64_t *out){
    if(!s||!s[0])return -1;
    for(const char *p=s;*p;p++)if(!((*p>='0'&&*p<='9')||(base==16&&*p>='a'&&*p<='f')))return -1;
    errno=0;char *end;unsigned long long v=strtoull(s,&end,(int)base);
    if(errno||*end||v>max)return -1;
    *out=v;return 0;
}
static int feature(const char *s,float *out){
    if(!s||!s[0])return -1;
    errno=0;char *end;float v=strtof(s,&end);
    if(errno||*end||!isfinite(v)||v<0||v>1)return -1;
    *out=v;return 0;
}
static int row(char *line,int outcomes,AllocatorData *d){
    char *field[16],*next=line;unsigned nf=0;
    while(next&&nf<16)field[nf++]=strsep(&next,"\t");
    if(next||nf!=(outcomes?15u:13u))return -1;
    uint64_t v[8],cost,age,mask=0;
    /* episode, task, family, cursor, population, jobs, work, wait_limit,
     * f0, f1, f2, cost, age [, coverage_mask, receipt] : 15 outcome columns. */
    for(unsigned i=0;i<8;i++)if(integer(field[i],i<2?UINT64_MAX:65535,10,v+i))return -1;
    if(!v[0]||!v[1]||v[2]>=4||!v[4]||v[4]>64||!v[5]||v[5]>8||!v[6]||v[6]>2048||!v[7])return -1;
    if(integer(field[11],2048,10,&cost)||integer(field[12],65535,10,&age)||!cost||cost>v[6])return -1;
    float f[3];for(unsigned i=0;i<3;i++)if(feature(field[8+i],f+i))return -1;
    if(outcomes&&(integer(field[13],UINT64_MAX,16,&mask)||strlen(field[14])!=64))return -1;
    if(!d->n||d->episode[d->n-1].id!=v[0]){
        if(d->n==256||(d->n&&d->episode[d->n-1].id>=v[0]))return -1;
        CnetAllocatorEpisode *e=d->episode+d->n++;
        e->id=v[0];e->family=(unsigned)v[2];e->cursor=(unsigned)v[3];e->population=(unsigned)v[4];
        e->jobs=(unsigned)v[5];e->work=(unsigned)v[6];e->wait_limit=(unsigned)v[7];
    }
    CnetAllocatorEpisode *e=d->episode+d->n-1;
    if(e->n==32||d->rows==2048||e->family!=v[2]||e->cursor!=v[3]||e->population!=v[4]||e->jobs!=v[5]||e->work!=v[6]||e->wait_limit!=v[7])return -1;
    unsigned index=e->n++;
    e->task[index]=(CnetAllocatorTask){.id=v[1],.cost=(unsigned)cost,.age=(unsigned)age};
    memcpy(e->task[index].features,f,sizeof f);e->coverage[index]=mask;
    if(outcomes)memcpy(e->receipt[index],field[14],64);
    d->rows++;return 0;
}
int allocator_data_read(const char *path,int outcomes,AllocatorData *out){
    if(!out)return -1;
    memset(out,0,sizeof *out);if(!path||(outcomes!=0&&outcomes!=1))return -1;
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);if(fd<0)return -1;
    struct stat s;char *bytes=NULL;int rc=-1;
    if(fstat(fd,&s)||!S_ISREG(s.st_mode)||s.st_uid!=geteuid()||(s.st_mode&077)||s.st_nlink!=1||s.st_size<1||s.st_size>1024*1024)goto done;
    size_t size=(size_t)s.st_size,used=0;bytes=calloc(size+1,1);if(!bytes)goto done;
    while(used<size){ssize_t got=read(fd,bytes+used,size-used);if(got<0&&errno==EINTR)continue;if(got<=0)goto done;used+=(size_t)got;}
    char extra;if(read(fd,&extra,1)!=0||memchr(bytes,0,size)||bytes[size-1]!='\n')goto done;
    if(cce_sha256_bytes_hex((unsigned char*)bytes,size,out->sha256))goto done;
    char *next=bytes,*line=strsep(&next,"\n");
    if(strcmp(line,outcomes?"CNET_ALLOCATOR_ROWS_V1":"CNET_ALLOCATOR_TASKS_V1"))goto done;
    while(next&&*next){line=strsep(&next,"\n");if(strlen(line)>511||row(line,outcomes,out))goto done;}
    if(!out->n)goto done;
    for(size_t i=0;i<out->n;i++){
        CnetAllocatorEpisode *e=out->episode+i;CnetAllocatorChoice p;
        if(outcomes?cnet_core_allocator_episode_validate(e):cnet_core_allocator_choose(NULL,e->task,e->n,e->cursor,e->jobs,e->work,e->wait_limit,1,&p))goto done;
    }
    rc=0;
done:free(bytes);if(close(fd))rc=-1;if(rc)memset(out,0,sizeof *out);return rc;
}
int allocator_data_disjoint(const AllocatorData *a,const AllocatorData *b){
    if(!a||!b||!a->n||!b->n||a->n>256||b->n>256||!strcmp(a->sha256,b->sha256))return -1;
    for(size_t i=0;i<a->n;i++)for(size_t j=0;j<b->n;j++){
        if(a->episode[i].id==b->episode[j].id)return -1;
        for(unsigned x=0;x<a->episode[i].n;x++)for(unsigned y=0;y<b->episode[j].n;y++)
            if(!strcmp(a->episode[i].receipt[x],b->episode[j].receipt[y]))return -1;
    }
    return 0;
}
