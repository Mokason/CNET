#define _GNU_SOURCE
#include "cnet_core_candidate.h"
#include "cce/cce_campaign_provenance.h"
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void){
    char dir[]="/tmp/cnet-candidate-XXXXXX";assert(mkdtemp(dir));int fd=open(dir,O_DIRECTORY|O_RDONLY|O_CLOEXEC);assert(fd>=0);
    CnetCoreCandidate c={0},copy;for(unsigned i=0;i<41;i++)c.cell.weight[i]=(float)i/41;
    memset(c.training_sha256,'1',64);memset(c.evaluation_sha256,'2',64);
    assert(!cnet_core_candidate_save_at(fd,"one.core",&c));assert(!cnet_core_candidate_load_at(fd,"one.core",&copy));
    assert(!memcmp(&c,&copy,sizeof c));assert(cnet_core_candidate_save_at(fd,"one.core",&c)!=0);
    assert(cnet_core_candidate_load_at(fd,"../one.core",&copy)!=0);
    assert(!symlinkat("one.core",fd,"link"));assert(cnet_core_candidate_load_at(fd,"link",&copy)!=0);
    assert(!mkfifoat(fd,"fifo",0600));assert(cnet_core_candidate_load_at(fd,"fifo",&copy)!=0);
    int file=openat(fd,"one.core",O_RDWR);assert(file>=0);unsigned char bytes[CNET_CORE_CANDIDATE_BYTES];
    assert(read(file,bytes,sizeof bytes)==sizeof bytes);
    assert(!memcmp(bytes,"CNETCEL1",8)&&bytes[8]==1&&bytes[24]==3&&bytes[28]==8&&bytes[40]==0);
    assert(!linkat(fd,"one.core",fd,"hard",0));assert(cnet_core_candidate_load_at(fd,"one.core",&copy)!=0);assert(!unlinkat(fd,"hard",0));
    const unsigned semantic_offsets[]={8,12,16,20,24,28,32,36,40,44,48,112,176};
    for(unsigned i=0;i<sizeof semantic_offsets/sizeof *semantic_offsets;i++){
        unsigned char changed[sizeof bytes];memcpy(changed,bytes,sizeof bytes);unsigned offset=semantic_offsets[i];
        if(offset==176){changed[offset]=0;changed[offset+1]=0;changed[offset+2]=0xc0;changed[offset+3]=0x7f;}
        else if(offset==48||offset==112)changed[offset]='z';else changed[offset]++;
        char hash[65];assert(!cce_sha256_bytes_hex(changed,340,hash));memcpy(changed+340,hash,64);
        assert(pwrite(file,changed,sizeof changed,0)==sizeof changed);assert(cnet_core_candidate_load_at(fd,"one.core",&copy)!=0);
        assert(pwrite(file,bytes,sizeof bytes,0)==sizeof bytes);
    }
    for(unsigned i=0;i<sizeof bytes;i++){
        unsigned char changed=bytes[i]^1;assert(pwrite(file,&changed,1,i)==1);
        assert(cnet_core_candidate_load_at(fd,"one.core",&copy)!=0);assert(pwrite(file,bytes+i,1,i)==1);
    }
    assert(!fchmod(file,0644));assert(cnet_core_candidate_load_at(fd,"one.core",&copy)!=0);assert(!fchmod(file,0600));
    assert(!ftruncate(file,sizeof bytes-1));assert(cnet_core_candidate_load_at(fd,"one.core",&copy)!=0);
    assert(pwrite(file,bytes,sizeof bytes,0)==sizeof bytes);assert(pwrite(file,"x",1,sizeof bytes)==1);
    assert(cnet_core_candidate_load_at(fd,"one.core",&copy)!=0);close(file);
    c.cell.weight[0]=NAN;assert(cnet_core_candidate_save_at(fd,"nan.core",&c)!=0);
    assert(!unlinkat(fd,"one.core",0)&&!unlinkat(fd,"link",0)&&!unlinkat(fd,"fifo",0));close(fd);assert(!rmdir(dir));
    puts("CORE_CANDIDATE_PASS canonical_roundtrip=1 every_byte_corruption_refused=1 path_symlink_fifo_permissions_truncation_trailing_nan_refused=1");
}
