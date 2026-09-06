#define main investigation_main
#include "investigate.c"
#undef main
#include <openssl/sha.h>
#define CONFIRM 2048
static Task confirmation[CONFIRM];
static Net snapshot;
static void freeze(const char *original) {
    uint64_t old[9216],fresh[CONFIRM];int used=0;char path[4096];
    const char *splits[]={"train","validation","test"};
    for(int s=0;s<3;s++) {
        int n=s?512:8192;
        assert(snprintf(path,sizeof path,"%s/%s.tsv",original,splits[s])<(int)sizeof path);
        read_tasks(path,training_tasks,n);
        for(int i=0;i<n;i++)old[used++]=effective_graph(training_tasks+i);
    }
    qsort(old,9216,sizeof *old,key_compare);
    uint32_t generator=96062026;FILE *out=fopen("confirmation.tsv","wx");assert(out);
    for(int i=0;i<CONFIRM;i++) {
        Task t;uint64_t key;int duplicate;
        do {
            task_generate(&t,&generator);key=effective_graph(&t);duplicate=degree_signature(key)%5!=0||bsearch(&key,old,9216,sizeof key,key_compare)!=NULL;
            for(int j=0;j<i&&!duplicate;j++)duplicate=fresh[j]==key;
        }while(duplicate);
        fresh[i]=key;uint64_t e=0,c=0;
        for(int j=0;j<64;j++){e|=(uint64_t)t.edge[j]<<j;c|=(uint64_t)t.compatible[j]<<j;}
        assert(fprintf(out,"%016" PRIx64 " %016" PRIx64 " %d %d\n",e,c,t.start,t.goal)>0);
    }
    assert(!fclose(out));puts("CONTROLLER_CONFIRMATION_FROZEN cases=2048 effective_graphs_disjoint=1 degree_signature_partition=0 seed=96062026");
}
int main(int argc,char **argv) {
    if(argc==3&&!strcmp(argv[1],"freeze")){freeze(argv[2]);return 0;}
    assert(argc==7&&!strcmp(argv[1],"evaluate"));char *end;
    long variant=strtol(argv[3],&end,10);assert(!*end&&variant>=0&&variant<8);
    unsigned seed=(unsigned)strtoul(argv[4],&end,10);assert(!*end&&seed>=1&&seed<=3);
    long mask=strtol(argv[5],&end,10);assert(!*end&&(mask==0||mask==1));
    assert(!strcmp(argv[6],"validation")||!strcmp(argv[6],"confirmation"));
    int count=!strcmp(argv[6],"validation")?512:CONFIRM;
    assert(!close_range(3,~0u,0));char file[64];snprintf(file,sizeof file,"%s.tsv",argv[6]);read_tasks(file,confirmation,count);
    FILE *f=fopen(argv[2],"rb");assert(f);assert(fread(&candidate,sizeof candidate,1,f)==1);assert(fgetc(f)==EOF);assert(!fclose(f));
    assert(candidate.depth==4&&candidate.tied==1);snapshot=candidate;
    unsigned char digest[SHA256_DIGEST_LENGTH];char hex[65];
    assert(SHA256((const unsigned char*)&candidate,sizeof candidate,digest));
    for(int i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",digest[i]);
    printf("{\"kind\":\"checkpoint\",\"split\":\"%s\",\"seed\":%u,\"mode\":\"%s\",\"mask\":%ld,\"loaded_sha256\":\"%s\"}\n",argv[6],seed,variants[variant],mask,hex);
    assert(!setenv("CNET_AMDMATH_MIN_FLOPS","0",1));char name[256];
    cce_amdmath *gpu=cce_amdmath_open_device(1,name,sizeof name);assert(gpu);
    assert(!offline_seal(-1)&&!offline_negative_test());
    int format=(variant==0||variant==3||variant==5)?0:variant==1?1:2;
    probe(confirmation,count,argv[6],format,seed,(int)variant,-1,(int)mask,stdout,gpu);
    assert(!memcmp(&candidate,&snapshot,sizeof candidate));
    cce_amdmath_close(gpu);return 0;
}
