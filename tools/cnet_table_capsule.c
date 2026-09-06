#include "cnet_capsule_table.h"
#include "cnet_learning_sandbox.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
static int number(const char *text,unsigned minimum,unsigned maximum,unsigned *out) {
    if(!text[0]||(text[0]=='0'&&text[1]))return -1;
    unsigned value=0;
    for(const unsigned char *p=(const unsigned char *)text;*p;p++) {
        if(*p<'0'||*p>'9')return -1;
        unsigned digit=*p-'0';
        if(value>maximum/10||(value==maximum/10&&digit>maximum%10))return -1;
        value=value*10+digit;
    }
    if(value<minimum)return -1;
    *out=value;return 0;
}
int main(int argc,char **argv) {
    if(argc==5&&!strcmp(argv[1],"build"))return cnet_capsule_table_build(argv[2],argv[3],argv[4]);
    if(argc==8&&!strcmp(argv[1],"build-worker")){
        char output[4096];
        unsigned parent,cpu,memory;
        if(number(argv[5],2,INT_MAX,&parent)||number(argv[6],1,120,&cpu)||
           number(argv[7],64,2048,&memory)){
            fprintf(stderr,"TABLE_CAPSULE_REFUSED worker_arguments\n");return 1;
        }
        int n=snprintf(output,sizeof output,"%s/capsule",argv[4]);
        if(n<0||n>=(int)sizeof output)return 1;
        if(cnet_learning_sandbox_enter_for_parent(argv[4],cpu,(size_t)memory*1024*1024,
                                                16u*1024u*1024u,(int)parent)){
            fprintf(stderr,"TABLE_CAPSULE_REFUSED worker_sandbox\n");return 1;
        }
        return cnet_capsule_table_build(argv[2],argv[3],output);
    }
    fprintf(stderr,"usage: %s build ABS_DATA_ROOT DATASET NEW_ABS_OUTPUT\n"
        "       %s build-worker ABS_DATA_ROOT DATASET EMPTY_ABS_PRIVATE_OUTPUT_ROOT EXPECTED_PARENT CPU_SECONDS MEMORY_MIB\n",argv[0],argv[0]);
    return 1;
}
