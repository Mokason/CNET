#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cnet_capsule_table.h"
#include "cce/cce_campaign_provenance.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int refuse(char *error,size_t cap,const char *why) {
    if(error&&cap)snprintf(error,cap,"%s",why);
    return -1;
}
int cnet_capsule_table_dataset(const char *s) {
    size_t n=s?strlen(s):0;
    if(!n||n>31||s[0]<'a'||s[0]>'z')return 0;
    for(size_t i=1;i<n;i++)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='0'&&s[i]<='9')||s[i]=='_'))return 0;
    return 1;
}
static int literal(const char **p,const char *s) {
    size_t n=strlen(s);if(strncmp(*p,s,n))return -1;*p+=n;return 0;
}
static int number(const char **p,unsigned max,char end,unsigned *value) {
    const char *s=*p;unsigned n=0;
    if(*s<'0'||*s>'9'||(*s=='0'&&s[1]!=end))return -1;
    do {unsigned digit=(unsigned)(*s++-'0');if(n>(max-digit)/10||digit>max)return -1;n=n*10+digit;}
    while(*s>='0'&&*s<='9');
    if(*s!=end||n>max)return -1;
    *p=s+1;*value=n;return 0;
}
static int symbol_character(unsigned char c) {
    return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
        c=='_'||c=='.'||c==':'||c=='-';
}
static int symbol_row(const char **p,CnetCapsuleTable *t,unsigned row) {
    const char *key=*p,*end=strchr(key,'\t');
    size_t n=end?(size_t)(end-key):0;
    if(!n||n>CNET_CAPSULE_SYMBOL_MAX_KEY)return -1;
    for(size_t i=0;i<n;i++)if(!symbol_character((unsigned char)key[i]))return -1;
    if(row){
        size_t previous=t->symbol_key_lengths[row-1],common=n<previous?n:previous;
        int order=memcmp(t->source+t->symbol_key_offsets[row-1],key,common);
        if(order>0||(!order&&previous>=n))return -1;
    }
    t->symbol_key_offsets[row]=(unsigned short)(key-t->source);
    t->symbol_key_lengths[row]=(unsigned char)n;
    const char *label=end+1;end=strchr(label,'\n');n=end?(size_t)(end-label):0;
    if(!n||n>CNET_CAPSULE_SYMBOL_MAX_LABEL)return -1;
    for(size_t i=0;i<n;i++)if((unsigned char)label[i]<32||(unsigned char)label[i]>126)return -1;
    t->symbol_label_offsets[row]=(unsigned short)(label-t->source);
    t->symbol_label_lengths[row]=(unsigned char)n;
    t->keys[row]=(unsigned char)row;t->values[row]=(unsigned short)row;
    *p=end+1;return 0;
}
static int decode(const void *asset,size_t length,CnetCapsuleTable *t) {
    memset(t,0,sizeof *t);
    if(!asset||!length||length>CNET_CAPSULE_TABLE_MAX_BYTES||memchr(asset,0,length))return -1;
    memcpy(t->source,asset,length);t->length=length;
    const char *p=t->source;
    if(!strncmp(p,"CNET_LOCAL_SYMBOLS_V1\n",22)){
        t->symbolic=1;
        if(literal(&p,"CNET_LOCAL_SYMBOLS_V1\ndataset "))return -1;
    } else if(literal(&p,"CNET_LOCAL_TABLE_V1\ndataset "))return -1;
    const char *end=strchr(p,'\n');size_t n=end?(size_t)(end-p):0;
    if(!n||n>=sizeof t->dataset)return -1;
    memcpy(t->dataset,p,n);p=end+1;
    if(!cnet_capsule_table_dataset(t->dataset)||literal(&p,"authority "))return -1;
    end=strchr(p,'\n');n=end?(size_t)(end-p):0;
    if(!n||n>=sizeof t->authority)return -1;
    memcpy(t->authority,p,n);p=end+1;
    if((strcmp(t->authority,"user_correction")&&strcmp(t->authority,"verified_tool"))||
       literal(&p,"input_bits 8\noutput_bits 16\nrows ")||
       number(&p,256,'\n',&t->count)||!t->count)return -1;
    for(unsigned i=0;i<t->count;i++){
        if(t->symbolic){if(symbol_row(&p,t,i))return -1;continue;}
        unsigned x,y;
        if(number(&p,255,'\t',&x)||number(&p,65535,'\n',&y)||(i&&x<=t->keys[i-1]))return -1;
        t->keys[i]=(unsigned char)x;t->values[i]=(unsigned short)y;
    }
    if(*p||cce_sha256_bytes_hex(asset,length,t->sha256))return -1;
    t->input=(Port){PORT_BINARY_MSB,8,1,""};t->output=(Port){PORT_BINARY_MSB,16,1,""};
    /* Distinct three-letter suffixes preserve CNB's near-miss typo refusal. */
    snprintf(t->input.tag,sizeof t->input.tag,"data_%.22s_key",t->sha256);
    snprintf(t->output.tag,sizeof t->output.tag,"data_%.22s_val",t->sha256);
    snprintf(t->unit,sizeof t->unit,"table_%.56s",t->sha256);
    return 0;
}
int cnet_capsule_table_symbol_index(const CnetCapsuleTable *t,const char *token,unsigned *row) {
    if(row)*row=0;
    if(!t||!t->symbolic||!token||!row)return -1;
    size_t n=strnlen(token,CNET_CAPSULE_SYMBOL_MAX_KEY+1);
    if(!n||n>CNET_CAPSULE_SYMBOL_MAX_KEY)return -1;
    for(size_t i=0;i<n;i++)if(!symbol_character((unsigned char)token[i]))return -1;
    for(unsigned i=0;i<t->count;i++)if(n==t->symbol_key_lengths[i]&&
        !memcmp(token,t->source+t->symbol_key_offsets[i],n)){*row=i;return 0;}
    return -1;
}
int cnet_capsule_table_key_at(const CnetCapsuleTable *t,unsigned row,char *text,size_t cap) {
    if(text&&cap)text[0]=0;
    if(!t||!t->symbolic||row>=t->count||!text||cap<=t->symbol_key_lengths[row])return -1;
    size_t n=t->symbol_key_lengths[row];
    memcpy(text,t->source+t->symbol_key_offsets[row],n);text[n]=0;return 0;
}
int cnet_capsule_table_render(const CnetCapsuleTable *t,unsigned row,unsigned value,char *text,size_t cap) {
    if(text&&cap)text[0]=0;
    if(!t||!t->symbolic||row>=t->count||value!=row||!text||cap<=t->symbol_label_lengths[row])return -1;
    size_t n=t->symbol_label_lengths[row];
    memcpy(text,t->source+t->symbol_label_offsets[row],n);text[n]=0;return 0;
}
/* Ancestors are traversed without symlinks; the configured root itself is
 * owner-private. Shared system ancestors such as /tmp need not be owned. */
static int open_root(const char *root) {
    char path[4096];
    if(!root||root[0]!='/'||!root[1]||strlen(root)>=sizeof path)return -1;
    strcpy(path,root+1);int fd=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)return -1;
    for(char *part=path;part;){
        char *slash=strchr(part,'/');if(slash)*slash=0;
        if(!*part||!strcmp(part,".")||!strcmp(part,"..")){close(fd);return -1;}
        struct stat parent;
        if(fstat(fd,&parent)||(parent.st_uid!=0&&parent.st_uid!=geteuid())||
           ((parent.st_mode&0022)&&!(parent.st_mode&S_ISVTX))){close(fd);return -1;}
        int next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        close(fd);fd=next;if(fd<0)return -1;
        part=slash?slash+1:NULL;
    }
    struct stat st;
    if(fstat(fd,&st)||st.st_uid!=geteuid()||(st.st_mode&0077)){close(fd);return -1;}
    return fd;
}
int cnet_capsule_table_read(const char *root,const char *dataset,CnetCapsuleTable *out,char *error,size_t cap) {
    if(error&&cap)error[0]=0;
    if(!out)return refuse(error,cap,"table_arguments");
    memset(out,0,sizeof *out);
    if(!cnet_capsule_table_dataset(dataset))return refuse(error,cap,"table_dataset");
    int dir=open_root(root);if(dir<0)return refuse(error,cap,"table_root_ownership_or_path");
    char leaf[40];snprintf(leaf,sizeof leaf,"%s.tsv",dataset);
    int fd=openat(dir,leaf,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);close(dir);
    if(fd<0)return refuse(error,cap,"table_source_open");
    struct stat before,after;int bad=fstat(fd,&before);
    if(!bad)bad=!S_ISREG(before.st_mode)||before.st_nlink!=1||before.st_uid!=geteuid()||
        (before.st_mode&0177)||before.st_size<1||before.st_size>CNET_CAPSULE_TABLE_MAX_BYTES;
    char bytes[CNET_CAPSULE_TABLE_MAX_BYTES+1];size_t used=0,n=bad?0:(size_t)before.st_size;
    while(!bad&&used<n){ssize_t got=read(fd,bytes+used,n-used);if(got<0&&errno==EINTR)continue;
        if(got<=0){bad=1;break;}used+=(size_t)got;}
    char extra;
    if(!bad)bad=read(fd,&extra,1)!=0||fstat(fd,&after)||before.st_dev!=after.st_dev||
        before.st_ino!=after.st_ino||before.st_size!=after.st_size||before.st_uid!=after.st_uid||
        before.st_mode!=after.st_mode||before.st_nlink!=after.st_nlink||
        before.st_mtim.tv_sec!=after.st_mtim.tv_sec||before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec||
        before.st_ctim.tv_sec!=after.st_ctim.tv_sec||before.st_ctim.tv_nsec!=after.st_ctim.tv_nsec;
    if(close(fd))bad=1;
    if(bad)return refuse(error,cap,"table_source_ownership_size_or_mutation");
    if(decode(bytes,n,out)||strcmp(out->dataset,dataset)){
        memset(out,0,sizeof *out);return refuse(error,cap,"table_noncanonical_source");
    }
    return 0;
}
static int port_eq(Port a,Port b) {
    return a.family==b.family&&a.field_width==b.field_width&&a.field_count==b.field_count&&!strcmp(a.tag,b.tag);
}
static int rows_eq(const CnetCapsuleTable *t,const double *in,const double *out,size_t count) {
    if(!in||!out||count!=t->count)return 0;
    for(unsigned i=0;i<t->count;i++){
        for(unsigned b=0;b<8;b++)if(in[i*8+b]!=((t->keys[i]>>(7-b))&1u))return 0;
        for(unsigned b=0;b<16;b++)if(out[i*16+b]!=((t->values[i]>>(15-b))&1u))return 0;
    }
    return 1;
}
CnetCapsuleTable *cnet_capsule_table_parse(const void *asset,size_t length,const Contract *c,
    const HybridCoverage *h,char *error,size_t cap) {
    if(error&&cap)error[0]=0;
    CnetCapsuleTable *t=malloc(sizeof *t);
    if(!t){refuse(error,cap,"table_memory");return NULL;}
    if(decode(asset,length,t)||!c||!h||strcmp(c->name,t->unit)||strcmp(c->name,h->unit)||
       c->input_port_count!=1||c->output_port_count!=1||!port_eq(c->input_ports[0],t->input)||
       !port_eq(c->output_ports[0],t->output)||!rows_eq(t,c->inputs,c->outputs,c->exemplar_count)||
       !h->active||h->generalizes||h->in_dim!=8||h->out_dim!=16||
       !port_eq(h->input_port,t->input)||!port_eq(h->goal_port,t->output)||!rows_eq(t,h->rows,h->targets,h->n_rows)){
        free(t);refuse(error,cap,"table_asset_or_label_binding");return NULL;
    }
    return t;
}
int cnet_capsule_table_fresh(const CnetCapsuleTable *t,const char *root) {
    CnetCapsuleTable current;
    return !t||cnet_capsule_table_read(root,t->dataset,&current,NULL,0)||strcmp(t->sha256,current.sha256)?-1:0;
}
int cnet_capsule_table_compatible(const CnetCapsuleTable *a,const CnetCapsuleTable *b) {
    if(!a||!b)return -1;
    /* Reserve both directions: a short digest collision must never join two
     * independently typed versions, even if its current row labels agree. */
    if(!strcmp(a->input.tag,b->input.tag)||!strcmp(a->output.tag,b->output.tag))
        return strcmp(a->sha256,b->sha256)?-1:0;
    return 0;
}

static int output_parent(const char *path,char leaf[97]) {
    char copy[4096];
    if(!path||path[0]!='/'||strlen(path)>=sizeof copy)return -1;
    strcpy(copy,path);char *last=strrchr(copy,'/');
    if(!last||last==copy||!last[1]||strlen(last+1)>96||last[1]=='.')return -1;
    strcpy(leaf,last+1);*last=0;
    for(const char *s=leaf;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||
        (*s>='0'&&*s<='9')||*s=='_'||*s=='-'||*s=='.'))return -1;
    return open_root(copy);
}
static int sync_output(int fd,const char *point) {
#ifdef CNET_TABLE_CAPSULE_TESTING
    const char *selected=getenv("CNET_TABLE_FAIL_SYNC");
    if(selected&&!strcmp(selected,point)){errno=EIO;return -1;}
#else
    (void)point;
#endif
    return fsync(fd);
}
int cnet_capsule_table_build(const char *root,const char *dataset,const char *output) {
    CnetCapsuleTable table,*checked=NULL;
    CnetBase base,imported;HybridAi coverage,restored;
    BinaryTransformNetwork btn={0},roundtrip_btn={0};Contract contract={0},roundtrip_contract={0};
    cnb_init(&base);cnb_init(&imported);hybrid_ai_init(&coverage);hybrid_ai_init(&restored);
    void *asset=NULL;size_t asset_length=0;unsigned asset_schema=0;
    int rc=1,parent=-1,dir=-1,created=0,complete=0;
    char error[160]="table_arguments",leaf[97],pinned[128];
    double inputs[256*8],targets[256*16];CertifyReport cert={0};
    parent=output_parent(output,leaf);
    if(parent<0){strcpy(error,"table_output_ownership_or_path");goto done;}
    if(cnet_capsule_table_read(root,dataset,&table,error,sizeof error))goto done;
    strcpy(error,"table_finite_compile_or_certification");
    unsigned n=table.count;
    if(btn_init(&btn,8,16,n,n,.1,7)||btn_set_ports(&btn,table.input,table.output))goto done;
    /* Existing finite-domain compiler: exact binary detectors for supplied
     * keys. At 256 rows, off-row leakage stays below 256*exp(-16).
     * Contract certification checks every supplied row at the unchanged .05
     * margin; exact coverage refuses every key absent from the source. */
    for(unsigned i=0;i<n;i++){
        unsigned ones=0;
        for(unsigned b=0;b<8;b++){
            unsigned bit=(table.keys[i]>>(7-b))&1u;ones+=bit;
            inputs[i*8+b]=bit;btn.input_hidden[i*8+b]=bit?32.0:-32.0;
        }
        btn.hidden_bias[i]=32.0*(.5-ones);
        for(unsigned b=0;b<16;b++){
            unsigned bit=(table.values[i]>>(15-b))&1u;targets[i*16+b]=bit;
            btn.hidden_output_weights[b*n+i]=bit?32.0:0.0;
        }
    }
    for(unsigned b=0;b<16;b++)btn.output_bias[b]=-16.0;
    if(contract_init_borrowed(&contract,table.unit,&btn,inputs,targets,n)||
       btn_certify_robust(&btn,&contract,.05,&cert))goto done;
    strcpy(error,"table_seal_or_tag_identity");
    if(cnb_add_unit(&base,&btn,&contract,NULL))goto done;
    strcpy(error,"table_provenance_or_coverage");
    char provenance[32];snprintf(provenance,sizeof provenance,"table_%s",table.authority);
    if(cnb_add_oracle_desc(&base,provenance,table.authority,table.input,table.output)||
       cnb_set_unit_provenance(&base,table.unit,provenance)||
       hybrid_coverage_record(&coverage,table.input,table.output,table.unit,inputs,targets,n,8,16))goto done;
    checked=cnet_capsule_table_parse(table.source,table.length,&contract,&coverage.coverage[0],error,sizeof error);
    if(!checked||cnet_capsule_table_fresh(checked,root)){strcpy(error,"table_prepublication_freshness");goto done;}
    if(mkdirat(parent,leaf,0700)){strcpy(error,"table_output_collision_or_create");goto done;}
    created=1;dir=openat(parent,leaf,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0){strcpy(error,"table_output_open");goto done;}
    snprintf(pinned,sizeof pinned,"/proc/self/fd/%d/.",dir);
    CnetCapsuleReport report;
    if(cnet_capsule_export_asset(&base,&coverage,table.unit,pinned,table.source,table.length,
                                CNET_CAPSULE_TABLE_SCHEMA,&report)){
        snprintf(error,sizeof error,"table_export:%.130s",report.reject_reason);goto done;
    }
    strcpy(error,"table_roundtrip_certification");
    if(cnet_capsule_import_asset(&imported,&restored,pinned,&asset,&asset_length,&asset_schema,&report)||
       asset_schema!=CNET_CAPSULE_TABLE_SCHEMA||asset_length!=table.length||memcmp(asset,table.source,table.length)||
       cnb_get_unit(&imported,table.unit,&roundtrip_btn,&roundtrip_contract)||
       btn_certify_robust(&roundtrip_btn,&roundtrip_contract,.05,&cert))goto done;
    free(checked);checked=cnet_capsule_table_parse(asset,asset_length,&roundtrip_contract,&restored.coverage[0],error,sizeof error);
    if(!checked||cnet_capsule_table_fresh(checked,root)){strcpy(error,"table_final_freshness");goto done;}
    complete=1;
    if(sync_output(dir,"directory_commit")||sync_output(parent,"parent_commit")){
        fprintf(stderr,"TABLE_CAPSULE_COMMIT_UNCERTAIN retained=1\n");rc=3;goto done;
    }
    printf("TABLE_CAPSULE_PASS unit=%s dataset=%s authority=%s source_sha256=%s input=%s output=%s rows=%u margin=%.6f pending_activation=1\n",
        table.unit,table.dataset,table.authority,table.sha256,table.input.tag,table.output.tag,n,cert.min_margin);
    rc=0;
done:
    if(created&&!complete){
        if(dir>=0){unlinkat(dir,"unit.cnb",0);unlinkat(dir,"manifest.cknow",0);unlinkat(dir,CNET_CAPSULE_ASSET_FILE,0);}
        unlinkat(parent,leaf,AT_REMOVEDIR);
    }
    if(dir>=0)close(dir);
    if(parent>=0)close(parent);
    if(rc==1)fprintf(stderr,"TABLE_CAPSULE_REFUSED %s\n",error);
    free(checked);free(asset);contract_free(&contract);contract_free(&roundtrip_contract);
    btn_free(&btn);btn_free(&roundtrip_btn);cnb_free(&base);cnb_free(&imported);
    hybrid_ai_free(&coverage);hybrid_ai_free(&restored);return rc;
}
