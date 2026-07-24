#include "../include/cnet_serve_decode.h"
#include <stdio.h>
#include <string.h>
static int f;
#define C(c,m) do{if(!(c)){fprintf(stderr,"FAIL %s\n",m);f++;}}while(0)
int main(void){
    int picks[]={2,0,5};
    const char *labs[]={"calc","mem","web","file","wiki","final"};
    char buf[256], j[256];
    int n=cnet_serve_decode_picks(picks,3,labs,6,buf,sizeof buf);
    C(n>0 && strstr(buf,"web") && strstr(buf,"calc"),"labels");
    n=cnet_serve_decode_json(picks,3,labs,6,j,sizeof j);
    C(n>0 && strstr(j,"\"picks\":[2,0,5]"),"json");
    if(f) return 1;
    printf("CNET_SERVE_DECODE_PASS checks=2\n");
    return 0;
}
