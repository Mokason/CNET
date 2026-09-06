/* Approved finite-tool path discovery. No model predictions or shell calls. */
#include "cnet_capsule_tool_internal.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define POLICY_MAX 256u
#define PLAN_STATES 1024u
#define PLAN_HOPS 8u
typedef struct {
    char input[32], output[32], op[4];
    unsigned ib, ob, operand, lo, hi, original_lo, original_hi;
} ToolRule;
static int atom(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 31) return 0;
    for (size_t i=0; i<n; i++) if (!((s[i]>='a' && s[i]<='z') ||
        (s[i]>='A' && s[i]<='Z') || (s[i]>='0' && s[i]<='9') || s[i]=='_')) return 0;
    return 1;
}
static int read_policy(const char *path, ToolRule *rules, size_t *count) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    if (fd < 0) return -1;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || st.st_size > 65536) { close(fd); return -1; }
    FILE *f = fdopen(fd,"r");
    if (!f) { close(fd); return -1; }
    char line[512]; size_t bytes=0; int rc=-1;
    *count=0;
    while (fgets(line,sizeof line,f)) {
        size_t len=strlen(line); bytes+=len;
        if (!len || line[len-1]!='\n' || bytes>65536) goto done;
        char *p=line; while (*p==' ' || *p=='\t' || *p=='\r' || *p=='\n') p++;
        if (!*p || *p=='#') continue;
        char ib[8],ob[8],operand[8],lo[8],hi[8],extra;
        ToolRule r={0};
        if (*count==POLICY_MAX || sscanf(p,"%31s %31s %7s %7s %3s %7s %7s %7s %c",
            r.input,r.output,ib,ob,r.op,operand,lo,hi,&extra)!=8 ||
            !atom(r.input) || !atom(r.output) ||
            capsule_tool_integer(ib,&r.ib) || capsule_tool_integer(ob,&r.ob) ||
            !r.ib || r.ib>16 || !r.ob || r.ob>16 ||
            (strcmp(r.op,"mul") && strcmp(r.op,"xor")) ||
            capsule_tool_integer(operand,&r.operand) || capsule_tool_integer(lo,&r.lo) ||
            capsule_tool_integer(hi,&r.hi) || r.lo>r.hi || r.hi>=(1u<<r.ib)) goto done;
        for (size_t i=0; i<*count; i++) {
            ToolRule *old=&rules[i];
            if (!strcmp(old->input,r.input) && !strcmp(old->output,r.output)) goto done;
            const char *tags[]={old->input,old->output}; unsigned widths[]={old->ib,old->ob};
            for (size_t j=0;j<2;j++) if ((!strcmp(tags[j],r.input) && widths[j]!=r.ib) ||
                (!strcmp(tags[j],r.output) && widths[j]!=r.ob)) goto done;
        }
        if (!strcmp(r.input,r.output) && r.ib!=r.ob) goto done;
        r.original_lo=r.lo; r.original_hi=r.hi;
        /* These pure tools have a contiguous representable input interval.
         * Restrict evidence, not the certification floor, before block fitting. */
        unsigned outmax=(1u<<r.ob)-1;
        if (!strcmp(r.op,"mul") && r.operand) {
            unsigned max=outmax/r.operand;
            if (r.hi>max) r.hi=max;
        } else if (!strcmp(r.op,"xor")) {
            unsigned min=r.operand & ~outmax, max=min | outmax;
            if (r.lo<min) r.lo=min;
            if (r.hi>max) r.hi=max;
        }
        rules[(*count)++]=r;
    }
    if (!ferror(f) && *count) rc=0;
done:
    fclose(f); return rc;
}
typedef struct {
    char tag[32];
    unsigned width,value,depth;
    size_t parent,edge;
} ToolState;
static int search(const ToolRule *rules, size_t count, const char *input,
                  const char *goal, unsigned value) {
    ToolState states[PLAN_STATES]={0}; size_t used=1,work=65536,calls=0;
    snprintf(states[0].tag,sizeof states[0].tag,"%s",input); states[0].value=value;
    for (size_t i=0;i<count;i++) if (!strcmp(rules[i].input,input)) states[0].width=rules[i].ib;
    int overflow=0;
    for (size_t head=0;head<used;head++) {
        ToolState *s=&states[head];
        if (s->depth==PLAN_HOPS) continue;
        for (size_t i=0;i<count;i++) {
            if (!work--) goto exhausted;
            const ToolRule *r=&rules[i];
            if (strcmp(s->tag,r->input) || s->width!=r->ib ||
                s->value<r->original_lo || s->value>r->original_hi) continue;
            if (s->value<r->lo || s->value>r->hi) { overflow=1; continue; }
            unsigned out; calls++;
            if (capsule_tool_eval(r->op,r->operand,s->value,&out) || out>=(1u<<r->ob)) goto exhausted;
            if (!strcmp(r->output,goal)) {
                size_t edges[PLAN_HOPS],parents[PLAN_HOPS],depth=s->depth+1;
                edges[depth-1]=i; parents[depth-1]=head;
                for (size_t n=depth-1;n;n--) {
                    edges[n-1]=states[parents[n]].edge;
                    parents[n-1]=states[parents[n]].parent;
                }
                for (size_t n=0;n<depth;n++) {
                    const ToolRule *edge=&rules[edges[n]];
                    unsigned x=states[parents[n]].value,y;
                    if (capsule_tool_eval(edge->op,edge->operand,x,&y)) return 2;
                    printf("%s %s %u %u %s %u %u %u %u %u\n",edge->input,edge->output,
                        edge->ib,edge->ob,edge->op,edge->operand,edge->lo,edge->hi,x,y);
                }
                if (fflush(stdout) || ferror(stdout)) return 2;
                fprintf(stderr,"CAPSULE_TOOL_PLAN_PASS hops=%zu oracle_calls=%zu work=%zu\n",depth,calls+depth,65536-work);
                return 0;
            }
            int seen=0;
            for (size_t n=0;n<used;n++) {
                if (!work--) goto exhausted;
                if (!strcmp(states[n].tag,r->output) && states[n].width==r->ob && states[n].value==out) { seen=1; break; }
            }
            if (seen) continue;
            if (used==PLAN_STATES) goto exhausted;
            ToolState *next=&states[used++];
            snprintf(next->tag,sizeof next->tag,"%s",r->output);
            next->value=out; next->width=r->ob; next->depth=s->depth+1;
            next->parent=head; next->edge=i;
        }
    }
    fprintf(stderr,"CAPSULE_TOOL_PLAN_REFUSED %s\n",overflow ? "unrepresentable_output" : "no_approved_path_within_hop_limit");
    return overflow ? 2 : 3;
exhausted:
    fprintf(stderr,"CAPSULE_TOOL_PLAN_REFUSED work_or_state_budget\n"); return 2;
}
int capsule_tool_plan(int argc, char **argv) {
    ToolRule rules[POLICY_MAX]; size_t count; unsigned value;
    if (argc==3 && !strcmp(argv[1],"validate")) {
        if (read_policy(argv[2],rules,&count)) {
            fprintf(stderr,"CAPSULE_TOOL_PLAN_REFUSED invalid_policy\n"); return 2;
        }
        return 0;
    }
    if (argc!=6 || !atom(argv[3]) || !atom(argv[4]) ||
        capsule_tool_integer(argv[5],&value) || read_policy(argv[2],rules,&count)) {
        fprintf(stderr,"CAPSULE_TOOL_PLAN_REFUSED invalid_policy_or_request\n"); return 2;
    }
    return search(rules,count,argv[3],argv[4],value);
}
