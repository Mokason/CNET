#define _GNU_SOURCE
#include "fixture.h"
#include "offline.h"
#include "cnet_json_internal.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
static double now(void) { struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t)); return t.tv_sec+t.tv_nsec*1e-9; }
static int full_io(int fd,char *data,size_t len,int writing) {
    while (len) {
        ssize_t n=writing ? write(fd,data,len) : read(fd,data,len);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return -1;
        data+=n; len-=(size_t)n;
    }
    return 0;
}
static int content_action(const char *body) {
    JsonCursor check={(const unsigned char *)body};
    if (json_value(&check,0)) return -1;
    json_ws(&check); if (*check.p) return -1;
    JsonCursor c={(const unsigned char *)body}; json_ws(&c);
    if (*c.p++!='{') return -1;
    int action=-1, found=0;
    for (;;) {
        char key[128], text[64]; json_ws(&c);
        if (*c.p=='}') break;
        if (json_string(&c,key,sizeof key)) return -1;
        json_ws(&c); if (*c.p++!=':') return -1; json_ws(&c);
        if (!strcmp(key,"content")) {
            if (found++ || json_string(&c,text,sizeof text)) return -1;
            if (strlen(text)!=1 || text[0]<'0' || text[0]>'8') return -1;
            action=text[0]-'0';
        } else if (json_value(&c,1)) return -1;
        json_ws(&c); if (*c.p=='}') break;
        if (*c.p++!=',') return -1;
    }
    return action;
}
static int propose(int fd,const Task *t,int current,const uint8_t banned[9]) {
    char prompt[4096], body[8192], request[10000], header[8192], response[65537]={0}, grammar[10];
    int used=snprintf(prompt,sizeof prompt,"Find a directed path to node %d from current node %d. Nodes are 0 through 7. Each listed edge is source,destination,compatible. Only compatible=1 edges may be used. Unlisted edges do not exist. Reply with the next node on a shortest path, or 8 if no path exists. Edge list: ",t->goal,current);
    for (int i=0;i<64;i++) if (t->edge[i]) used+=snprintf(prompt+used,sizeof prompt-(size_t)used,"(%d,%d,%d) ",i/8,i%8,t->compatible[i]);
    int g=0; for (int a=0;a<9;a++) if (!banned[a]) grammar[g++]=(char)('0'+a); grammar[g]=0;
    int length=snprintf(body,sizeof body,"{\"prompt\":\"%s Allowed replies: %s. Answer:\",\"grammar\":\"root ::= [%s]\",\"n_predict\":1,\"temperature\":0,\"cache_prompt\":false,\"stream\":false}",prompt,grammar,grammar);
    assert(length>0 && length<(int)sizeof body);
    int total=snprintf(request,sizeof request,"POST /completion HTTP/1.1\r\nHost: 127.0.0.1:8092\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n%s",length,body);
    if (total<0 || total>=(int)sizeof request || full_io(fd,request,(size_t)total,1)) return -1;
    size_t n=0;
    do {
        if (n+1>=sizeof header || full_io(fd,header+n,1,0)) return -1;
        header[++n]=0;
    } while (n<4 || strcmp(header+n-4,"\r\n\r\n"));
    if (strncmp(header,"HTTP/1.1 200 ",13) || strcasestr(header,"\r\nTransfer-Encoding:")) return -1;
    const char *size=strcasestr(header,"\r\nContent-Length:");
    if (!size) return -1;
    char *end; unsigned long bytes=strtoul(size+17,&end,10);
    if (bytes==0 || bytes>=sizeof response || strncmp(end,"\r\n",2)) return -1;
    if (full_io(fd,response,bytes,0)) return -1;
    return content_action(response);
}
int main(int argc,char **argv) {
    assert(argc==2); char *end; long index=strtol(argv[1],&end,10); assert(!*end && index>=0 && index<32);
    assert(!close_range(3,~0u,0));
    FILE *f=fopen("test.tsv","r"); assert(f); Task t;
    for (int i=0;i<=index;i++) {
        uint64_t edges,types;
        assert(fscanf(f,"%" SCNx64 " %" SCNx64 " %d %d",&edges,&types,&t.start,&t.goal)==4);
        for (int j=0;j<64;j++) { t.edge[j]=(edges>>j)&1; t.compatible[j]=(types>>j)&1; }
    }
    fclose(f); assert(t.start>=0 && t.start<8 && t.goal>=0 && t.goal<8);
    int fd=socket(AF_INET,SOCK_STREAM,0); assert(fd>=0);
    struct timeval timeout={.tv_sec=30};
    assert(!setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout));
    assert(!setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout));
    struct sockaddr_in peer={.sin_family=AF_INET,.sin_port=htons(8092),.sin_addr={htonl(INADDR_LOOPBACK)}};
    assert(!connect(fd,(struct sockaddr *)&peer,sizeof peer));
    signal(SIGPIPE,SIG_IGN); alarm(180);
    assert(!offline_seal(fd) && !offline_negative_test());
    float y[9]; int dist=task_teacher(&t,t.start,y), current=t.start;
    int completed=0, optimal=0, invalid=0, attempts=0, abstained=0;
    uint8_t banned[8][9]={0}; double start=now();
    for (int step=0;step<8;step++) {
        int a=propose(fd,&t,current,banned[current]);
        if (a<0) { fprintf(stderr,"LOCAL_BASELINE_RED case=%ld step=%d transport_or_response\n",index,step); return 1; }
        if (!step) optimal=y[a]>0;
        attempts++; int checked=task_check(&t,current,a);
        printf("{\"kind\":\"local_action\",\"case\":%ld,\"step\":%d,\"current\":%d,\"proposal\":%d,\"check\":%d}\n",index,step,current,a,checked);
        if (a==8) { abstained=dist<0; break; }
        if (!checked) { invalid++; banned[current][a]=1; }
        else { current=a; if (checked==2) { completed=1; break; } }
    }
    close(fd);
    printf("{\"kind\":\"local_evaluation\",\"case\":%ld,\"distance\":%d,\"reachable\":%d,\"completed\":%d,\"optimal_first\":%d,\"attempts\":%d,\"illegal\":%d,\"unreachable_abstained\":%d,\"seconds\":%.6f,\"client_network_restricted\":true,\"server_isolated\":false}\n",index,dist,dist>0,completed,optimal,attempts,invalid,abstained,now()-start);
    return 0;
}
