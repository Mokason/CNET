#define _GNU_SOURCE
#include "allocator_data.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static const char *rows="CNET_ALLOCATOR_ROWS_V1\n1\t10\t0\t0\t16\t1\t32\t4\t0.5\t0.75\t0.015625\t32\t0\t00ff\taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
static void replace(const char *path,const char *bytes,size_t n){int fd=open(path,O_WRONLY|O_TRUNC);assert(fd>=0);assert(write(fd,bytes,n)==(ssize_t)n);assert(!close(fd));}
int main(void){
    char file[]="/tmp/cnet-allocator-rows-XXXXXX";int fd=mkstemp(file);assert(fd>=0);
    assert(write(fd,rows,strlen(rows))==(ssize_t)strlen(rows));close(fd);
    AllocatorData *d=calloc(1,sizeof *d);assert(d);
    if(allocator_data_read(file,1,d)){puts("CORE_ALLOCATOR_DATA_RED valid_supplied_outcomes_refused");return 1;}
    assert(d->n==1&&d->rows==1&&d->episode[0].coverage[0]==255);
    assert(allocator_data_read(file,0,d)); /* Outcomes cannot enter choose. */
    assert(!allocator_data_read(file,1,d));
    assert(allocator_data_disjoint(d,d));
    assert(!chmod(file,0644)&&allocator_data_read(file,1,d));assert(!chmod(file,0600));
    replace(file,rows,strlen(rows)-1);assert(allocator_data_read(file,1,d));
    char changed[1024];assert(strlen(rows)<sizeof changed);strcpy(changed,rows);
    char *f=strstr(changed,"0.5");assert(f);memcpy(f,"nan",3);
    replace(file,changed,strlen(changed));assert(allocator_data_read(file,1,d));
    strcpy(changed,rows);f=strstr(changed,"00ff");assert(f);memcpy(f,"fffff",5);
    replace(file,changed,strlen(changed));assert(allocator_data_read(file,1,d));
    strcpy(changed,rows);strcat(changed,strchr(rows,'\n')+1);
    replace(file,changed,strlen(changed));assert(allocator_data_read(file,1,d));
    replace(file,rows,strlen(rows));
    char link[128];snprintf(link,sizeof link,"%s-link",file);assert(!symlink(file,link));assert(allocator_data_read(link,1,d));assert(!unlink(link));
    strcpy(changed,rows);f=strstr(changed,"ROWS");assert(f);memmove(f+5,f+4,strlen(f+4)+1);memcpy(f,"TASKS",5);
    char *mask=strstr(changed,"\t00ff\t");assert(mask);strcpy(mask,"\n");
    replace(file,changed,strlen(changed));assert(!allocator_data_read(file,0,d)&&d->rows==1);
    free(d);assert(!unlink(file));puts("CORE_ALLOCATOR_DATA_PASS");
}
