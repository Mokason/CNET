#include <stdio.h>
#include <stdlib.h>
#include "../include/soul_host.h"
int main(void){
  SoulHost *h=NULL; SoulServeStats st;
  if(soul_open("soul_gemma4v2_final.cnb",NULL,&h)!=0){puts("open_fail");return 1;}
  soul_serve_stats(h,&st);
  printf("units=%d residual_bound=%d window=%d\n", st.units, st.residual_bound, st.residual_window);
  soul_close(h);
  puts(st.residual_bound?"SOUL_RESIDUAL_BOUND_OK":"SOUL_RESIDUAL_BOUND_FAIL");
  return st.residual_bound?0:2;
}
