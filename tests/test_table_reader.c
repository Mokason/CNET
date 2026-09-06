#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cnet_capsule_table.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int failures,checks;
static void check(int ok,const char *why){checks++;if(!ok){failures++;fprintf(stderr,"TABLE_READER_RED %s\n",why);}}
int main(void){
    char root[]="/tmp/cnet-table-reader-XXXXXX",path[256],error[160];
    if(!mkdtemp(root))return 2;
    snprintf(path,sizeof path,"%s/stock.tsv",root);
    const char raw[]="CNET_LOCAL_TABLE_V1\ndataset stock\nauthority user_correction\ninput_bits 8\noutput_bits 16\nrows 2\n0\t65535\n255\t0\n";
    int fd=open(path,O_CREAT|O_EXCL|O_WRONLY,0600);
    if(fd<0||write(fd,raw,sizeof raw-1)!=(ssize_t)(sizeof raw-1)||close(fd))return 2;
    CnetCapsuleTable t;
    check(!cnet_capsule_table_read(root,"stock",&t,error,sizeof error),"canonical private table reads");
    check(t.count==2&&t.keys[0]==0&&t.values[0]==65535&&t.keys[1]==255&&t.values[1]==0,"endpoint row values");
    check(strlen(t.sha256)==64&&strlen(t.input.tag)==31&&strlen(t.output.tag)==31,"full hash and bounded interface tags");
    check(!cnet_capsule_table_fresh(&t,root),"fresh owner source");
    double in[16],out[32];
    for(unsigned i=0;i<2;i++){
        for(unsigned b=0;b<8;b++)in[i*8+b]=(t.keys[i]>>(7-b))&1u;
        for(unsigned b=0;b<16;b++)out[i*16+b]=(t.values[i]>>(15-b))&1u;
    }
    Contract c={0};HybridCoverage h={0};
    snprintf(c.name,sizeof c.name,"%s",t.unit);snprintf(h.unit,sizeof h.unit,"%s",t.unit);
    c.input_port_count=c.output_port_count=1;c.input_ports[0]=h.input_port=t.input;c.output_ports[0]=h.goal_port=t.output;
    c.inputs=h.rows=in;c.outputs=h.targets=out;c.exemplar_count=h.n_rows=2;h.in_dim=8;h.out_dim=16;h.active=1;
    CnetCapsuleTable *parsed=cnet_capsule_table_parse(raw,sizeof raw-1,&c,&h,error,sizeof error);
    check(parsed!=NULL,"asset binds complete contract and coverage");free(parsed);
    out[0]=0;parsed=cnet_capsule_table_parse(raw,sizeof raw-1,&c,&h,error,sizeof error);
    check(!parsed,"changed output labels refuse");free(parsed);out[0]=1;
    h.generalizes=1;parsed=cnet_capsule_table_parse(raw,sizeof raw-1,&c,&h,error,sizeof error);
    check(!parsed,"widened coverage refuses");free(parsed);h.generalizes=0;
    CnetCapsuleTable collision=t;collision.sha256[63]=collision.sha256[63]=='0'?'1':'0';
    check(cnet_capsule_table_compatible(&t,&collision)!=0,"short interface tag collision refuses full hash mismatch");
    strcpy(collision.input.tag,"data_distinct_i");strcpy(collision.output.tag,"data_distinct_o");
    check(!cnet_capsule_table_compatible(&t,&collision),"distinct version interfaces coexist");
    unlink(path);check(cnet_capsule_table_fresh(&t,root)!=0,"deleted source becomes stale");rmdir(root);
    printf("TABLE_READER_%s checks=%d failures=%d\n",failures?"RED":"PASS",checks,failures);return failures?1:0;
}
