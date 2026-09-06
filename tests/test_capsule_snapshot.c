/* Real RED first: secure immutable inventory acquisition, no new capsule format. */
#define main original_capsule_fixture_main
#include "test_knowledge_capsule.c"
#undef main
#include <dlfcn.h>
#include <fcntl.h>

int main(void) {
    const char *library=getenv("CNET_TEST_CORE_LIBRARY");
    void *lib=dlopen(library?library:"bin/libcnet_capsule_core.so",RTLD_NOW|RTLD_LOCAL);
    int (*snapshot)(int,int,const char *,char [65],size_t *);
    int (*verify)(int,const char *,size_t *);
    if(!lib){puts("CAPSULE_SNAPSHOT_RED missing_runtime");return 1;}
    *(void **)(&snapshot)=dlsym(lib,"cnet_capsule_snapshot_create");
    *(void **)(&verify)=dlsym(lib,"cnet_capsule_snapshot_verify");
    if(!snapshot||!verify){puts("CAPSULE_SNAPSHOT_RED missing_secure_acquisition");return 1;}
    char root[]="/tmp/cnet-snapshot-XXXXXX",source[256],dest[256],unit[300];
    if(!mkdtemp(root))return 2;
    snprintf(source,sizeof source,"%s/source",root);snprintf(dest,sizeof dest,"%s/snapshots",root);
    if(mkdir(source,0700)||mkdir(dest,0700))return 2;
    snprintf(unit,sizeof unit,"%s/offset",source);
    CnetBase b;HybridAi h;CnetCapsuleReport report;cnb_init(&b);hybrid_ai_init(&h);
    check(!build_unit(&b,&h,"offset")&&!cnet_capsule_export(&b,&h,"offset",unit,&report),"build independently labelled package");
    check(!chmod(unit,0700),"owner seals source directory permissions");
    int src=open(source,O_RDONLY|O_DIRECTORY),dst=open(dest,O_RDONLY|O_DIRECTORY);
    char digest[65]={0},again[65]={0};size_t bytes=0;
    check(src>=0&&dst>=0&&!snapshot(src,dst,NULL,digest,&bytes)&&bytes>0,"copy complete inventory through pinned descriptors");
    check(!snapshot(src,dst,NULL,again,&bytes)&&!strcmp(digest,again),"identical source resolves exact immutable identity");
    char payload[350],linkpath[350];snprintf(payload,sizeof payload,"%s/unit.cnb",unit);
    snprintf(linkpath,sizeof linkpath,"%s/unexpected",source);
    check(!symlink(unit,linkpath)&&snapshot(src,dst,NULL,again,&bytes)!=0,"symlink inventory member refuses");unlink(linkpath);
    snprintf(linkpath,sizeof linkpath,"%s/extra",unit);
    check(!link(payload,linkpath)&&snapshot(src,dst,NULL,again,&bytes)!=0,"hardlinked payload and unknown files refuse");unlink(linkpath);
    check(!mkfifo(linkpath,0600)&&snapshot(src,dst,NULL,again,&bytes)!=0,"FIFO artifact refuses without blocking");unlink(linkpath);
    check(snapshot(src,dst,"../escape",again,&bytes)!=0,"snapshot selection cannot escape owner directory");
    check(!verify(dst,digest,&bytes),"stored snapshot rehash verifies before recovery");
    snprintf(payload,sizeof payload,"%s/%s/offset/unit.cnb",dest,digest);
    chmod(payload,0600);
    FILE *fp=fopen(payload,"r+b");if(fp){fputc(0,fp);fclose(fp);}
    check(fp&&verify(dst,digest,&bytes)!=0,"changed durable snapshot cannot retain original identity");
    if(src>=0)close(src);if(dst>=0)close(dst);cnb_free(&b);hybrid_ai_free(&h);dlclose(lib);
    printf("CAPSULE_SNAPSHOT_%s checks=%d failures=%d artifacts=%s\n",failures?"RED":"PASS",checks,failures,root);
    return failures?1:0;
}
