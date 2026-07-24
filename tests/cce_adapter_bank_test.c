#include "../include/cce/cce_adapter_bank.h"
#include <stdio.h>
#include <string.h>
static int f; 
#define C(c,m) do{if(!(c)){fprintf(stderr,"FAIL %s\n",m);f++;} }while(0)
int main(void){
    cce_adapter_bank b; int k=0; void *h;
    int dummy1=1, dummy2=2;
    cce_adapter_bank_init(&b);
    C(cce_adapter_bank_put(&b,"skill_a",&dummy1,1,1)==0,"put a");
    C(cce_adapter_bank_put(&b,"skill_b",&dummy2,1,0)==1,"put b uncertified");
    C(cce_adapter_bank_select(&b,"skill_b")!=0,"reject uncertified");
    C(cce_adapter_bank_select(&b,"skill_a")==0,"select a");
    h=cce_adapter_bank_active(&b,&k);
    C(h==&dummy1 && k==1,"active a");
    cce_adapter_bank_clear_active(&b);
    C(cce_adapter_bank_active(&b,&k)==NULL,"cleared");
    if(f){fprintf(stderr,"fails=%d\n",f);return 1;}
    printf("CCE_ADAPTER_BANK_PASS checks=6\n");
    return 0;
}
