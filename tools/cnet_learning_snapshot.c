#include "../src/serve/cnet_capsule_snapshot.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Trusted parent authority, NOT a sealed evidence worker. This copies bounded
 * opaque artifact bytes using the existing canonical snapshot implementation;
 * it does not parse capsule semantics, certify answers, activate, or train.
 * The native snapshot ceiling is 32GiB; the supervisor enforces smaller quotas. */
static int refused(const char *reason) {
    fprintf(stderr,"CNET_LEARNING_SNAPSHOT_REFUSED %s\n",reason);return 1;
}
int main(int argc,char **argv) {
    if(argc!=4||strcmp(argv[1],"freeze")||argv[2][0]!='/'||argv[3][0]!='/'||
       strlen(argv[2])>4095||strlen(argv[3])>4095)return refused("arguments");
    int source=cnet_capsule_owner_directory(argv[2],1);
    if(source<0)return refused("source_boundary");
    int cache=cnet_capsule_owner_directory(argv[3],1);
    if(cache<0){close(source);return refused("cache_boundary");}
    char digest[65];size_t bytes=0,verified_bytes=0;
    int bad=cnet_capsule_snapshot_create(source,cache,NULL,digest,&bytes);
    if(!bad)bad=cnet_capsule_snapshot_verify(cache,digest,&verified_bytes)||bytes!=verified_bytes;
    if(close(source))bad=1;
    if(close(cache))bad=1;
    if(bad)return refused("freeze");
    if(printf("CNET_LEARNING_SNAPSHOT_V1\nsnapshot_sha256 %s\nbytes %zu\nend\n",digest,bytes)<0||fflush(stdout))
        return refused("output");
    return 0;
}
