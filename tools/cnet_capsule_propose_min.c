#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
static int mymkdir(const char *p){return mkdir(p,0755);}
int main(int argc,char**argv){
  const char*unit=0,*out_root=0; int i; char outdir[512],prop[640]; time_t now=time(0); FILE*f;
  for(i=1;i<argc;i++){
    if(!strcmp(argv[i],"--unit")&&i+1<argc) unit=argv[++i];
    else if(!strcmp(argv[i],"--out")&&i+1<argc) out_root=argv[++i];
  }
  if(!unit){fprintf(stderr,"usage: cnet_capsule_propose --unit NAME [--out DIR]\n");return 2;}
  if(!out_root) out_root="var/capsule_inbox";
  mymkdir(out_root);
  snprintf(outdir,sizeof outdir,"%s/%s-%ld",out_root,unit,(long)now);
  mymkdir(outdir);
  snprintf(prop,sizeof prop,"%s/PROPOSE.json",outdir);
  f=fopen(prop,"w"); if(!f) return 1;
  fprintf(f,"{\n  \"kind\": \"capsule_propose\",\n  \"unit\": \"%s\",\n  \"out\": \"%s\",\n  \"auto_cert\": false,\n  \"status\": \"pending_verify\",\n  \"export\": \"deferred\",\n  \"law\": \"propose_only_never_self_cert\",\n  \"ts\": %ld\n}\n", unit, outdir, (long)now);
  fclose(f);
  printf("CAPSULE_PROPOSE unit=%s out=%s auto_cert=0\n", unit, outdir);
  printf("CAPSULE_PROPOSE_PASS\n");
  return 0;
}
