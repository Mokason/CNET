#include "cnet_core_host.h"
#include "cce/cce_campaign_provenance.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
/* The legacy certificate cache and BTN scratch are mutable. One process-wide
 * lock covers all wrappers, including admission, replay and destruction. */
static pthread_mutex_t runtime_lock=PTHREAD_MUTEX_INITIALIZER;
typedef struct {
    CnetCapsuleCore *core;CnetCoreCandidate candidate;
    uint64_t id,checked_against;unsigned pins;int neural,approved;
    CnetCapsuleIdentity *known;size_t known_count;
} Generation;
struct CnetCoreHost {Generation *active,*rollback,*staged,*slot[4];uint64_t next,revision;unsigned pins;CnetCapsuleIdentity *known;size_t known_count;};
struct CnetCoreLease {CnetCoreHost *host;Generation *view;};
static void collect(CnetCoreHost *h){
    for(unsigned i=0;i<4;i++){Generation *g=h->slot[i];if(g&&g!=h->active&&g!=h->rollback&&g!=h->staged&&!g->pins){cnet_capsule_core_close(g->core);free(g->known);free(g);h->slot[i]=NULL;}}
}
/* Keep identity history across set switches and unload, bounded independently
 * from the 4096-unit active inventory. No replacement is inferred from replay. */
static int prepare_identities(CnetCoreHost *h,Generation *g) {
    CnetCapsuleIdentity *current=calloc(4096,sizeof *current);
    if (!current) return -1;
    size_t n=0;
    if (cnet_capsule_core_identities(g->core,current,4096,&n)) {
        free(current);return -1;
    }
    CnetCapsuleIdentity *merged=calloc(h->known_count+n+1,sizeof *merged);
    if (!merged) {free(current);return -1;}
    size_t i=0,j=0,k=0;
    while (i<h->known_count || j<n) {
        if(k==16384){free(current);free(merged);return -1;}
        int cmp=i==h->known_count?1:j==n?-1:strcmp(h->known[i].unit,current[j].unit);
        if (!cmp) {
            if (strcmp(h->known[i].sha256,current[j].sha256)) {free(current);free(merged);return -1;}
            merged[k++]=h->known[i++];j++;
        } else if (cmp<0) merged[k++]=h->known[i++];
        else merged[k++]=current[j++];
    }
    free(current);free(g->known);g->known=merged;g->known_count=k;return 0;
}
CnetCoreHost *cnet_core_host_open(const char *registry){
    if(!registry)return NULL;
    CnetCoreHost *h=calloc(1,sizeof *h);Generation *g=calloc(1,sizeof *g);
    if(!h||!g){free(h);free(g);return NULL;}
    pthread_mutex_lock(&runtime_lock);char error[160];size_t obligations=0;
    g->core=cnet_capsule_core_open(registry,error,sizeof error);
    int valid=g->core && !cnet_capsule_core_validate_growth(g->core,g->core,&obligations) && !prepare_identities(h,g);
    if (!valid) {cnet_capsule_core_close(g->core);free(g->known);}
    pthread_mutex_unlock(&runtime_lock);
    if(!valid){free(g);free(h);return NULL;}
    h->known=g->known;h->known_count=g->known_count;g->known=NULL;g->known_count=0;
    g->id=1;h->next=2;h->revision=1;h->active=h->slot[0]=g;return h;
}
int cnet_core_host_close(CnetCoreHost *h){
    if(!h)return -1;
    pthread_mutex_lock(&runtime_lock);if(h->pins){pthread_mutex_unlock(&runtime_lock);return -1;}
    h->active=h->rollback=h->staged=NULL;collect(h);free(h->known);pthread_mutex_unlock(&runtime_lock);free(h);return 0;
}
CnetCoreLease *cnet_core_host_pin(CnetCoreHost *h){
    if(!h)return NULL;
    CnetCoreLease *lease=malloc(sizeof *lease);if(!lease)return NULL;
    pthread_mutex_lock(&runtime_lock);if(h->pins==64){pthread_mutex_unlock(&runtime_lock);free(lease);return NULL;}
    lease->host=h;lease->view=h->active;h->pins++;h->active->pins++;pthread_mutex_unlock(&runtime_lock);return lease;
}
void cnet_core_host_unpin(CnetCoreLease *lease){
    if(!lease)return;
    pthread_mutex_lock(&runtime_lock);lease->host->pins--;lease->view->pins--;collect(lease->host);pthread_mutex_unlock(&runtime_lock);free(lease);
}
uint64_t cnet_core_host_generation(const CnetCoreLease *lease){return lease?lease->view->id:0;}
int cnet_core_host_ask(CnetCoreLease *lease,const char *request,CnetCapsuleCoreReply *reply){
    if(reply)memset(reply,0,sizeof *reply);
    if(!lease||!request||!reply)return -1;
    pthread_mutex_lock(&runtime_lock);Generation *g=lease->view;
    int rc=g->neural?cnet_capsule_core_ask_cell(g->core,request,&g->candidate.cell,g->id,reply):cnet_capsule_core_ask(g->core,request,reply);
    pthread_mutex_unlock(&runtime_lock);return rc;
}
int cnet_core_host_ask_text(CnetCoreLease *lease,const char *request,CnetCapsuleCoreReply *reply,char *text,size_t cap) {
    if(reply)memset(reply,0,sizeof *reply);
    if(text&&cap)text[0]=0;
    if(!lease||!request||!reply||!text||!cap)return -1;
    pthread_mutex_lock(&runtime_lock);Generation *g=lease->view;
    int rc=g->neural?cnet_capsule_core_ask_cell_text(g->core,request,&g->candidate.cell,g->id,reply,text,cap):
        cnet_capsule_core_ask_text(g->core,request,reply,text,cap);
    pthread_mutex_unlock(&runtime_lock);return rc;
}
uint64_t cnet_core_host_revision(CnetCoreHost *h) {
    if (!h) return 0;
    pthread_mutex_lock(&runtime_lock);uint64_t revision=h->revision;
    pthread_mutex_unlock(&runtime_lock);return revision;
}
int cnet_core_host_history(CnetCoreHost *h,uint64_t staged,CnetCapsuleIdentity *out,size_t capacity,size_t *count) {
    if(count)*count=0;
    if(!h||!count)return -1;
    pthread_mutex_lock(&runtime_lock);
    Generation *g=h->staged;int rc=-1;
    if(staged&&(!g||g->id!=staged||!g->approved||g->checked_against!=h->revision))goto done;
    const CnetCapsuleIdentity *known=staged?g->known:h->known;
    size_t n=staged?g->known_count:h->known_count;*count=n;
    if(n>capacity||(n&&!out))goto done;
    if(n)memcpy(out,known,n*sizeof *out);
    rc=0;
done:pthread_mutex_unlock(&runtime_lock);return rc;
}
int cnet_core_host_restore_history(CnetCoreHost *h,const CnetCapsuleIdentity *known,size_t count) {
    if(!h||count>16384||(count&&!known))return -1;
    for(size_t i=0;i<count;i++) {
        if(!known[i].unit[0]||!memchr(known[i].unit,0,64)||known[i].sha256[64]||
           (i&&strcmp(known[i-1].unit,known[i].unit)>=0))return -1;
        for(unsigned j=0;j<64;j++)if(!((known[i].sha256[j]>='0'&&known[i].sha256[j]<='9')||
            (known[i].sha256[j]>='a'&&known[i].sha256[j]<='f')))return -1;
    }
    CnetCapsuleIdentity *copy=calloc(count+1,sizeof *copy);if(!copy)return -1;
    if(count)memcpy(copy,known,count*sizeof *copy);
    pthread_mutex_lock(&runtime_lock);int rc=-1;
    if(h->revision!=1||h->pins||h->staged)goto done;
    size_t j=0;
    for(size_t i=0;i<h->known_count;i++) {
        while(j<count&&strcmp(known[j].unit,h->known[i].unit)<0)j++;
        if(j==count||strcmp(known[j].unit,h->known[i].unit)||strcmp(known[j].sha256,h->known[i].sha256))goto done;
    }
    free(h->known);h->known=copy;h->known_count=count;copy=NULL;rc=0;
done:pthread_mutex_unlock(&runtime_lock);free(copy);return rc;
}
int cnet_core_host_stage_registry(CnetCoreHost *h,const char *registry,int preserve,uint64_t *id) {
    if (id) *id=0;
    if (!h || !id || (preserve!=0 && preserve!=1)) return -1;
    pthread_mutex_lock(&runtime_lock);collect(h);
    unsigned slot=0;while(slot<4 && h->slot[slot])slot++;
    if (h->staged || slot==4 || h->next==UINT64_MAX) {pthread_mutex_unlock(&runtime_lock);return -1;}
    Generation *g=calloc(1,sizeof *g);char error[160];size_t obligations=0;
    if (g) g->core=registry?cnet_capsule_core_open(registry,error,sizeof error):cnet_capsule_core_empty();
    if (!g || !g->core || cnet_capsule_core_validate_growth(g->core,g->core,&obligations) ||
        (preserve && cnet_capsule_core_validate_growth(h->active->core,g->core,&obligations)) ||
        prepare_identities(h,g)) {
        if(g){cnet_capsule_core_close(g->core);free(g->known);free(g);}
        pthread_mutex_unlock(&runtime_lock);return -1;
    }
    g->id=h->next++;g->approved=1;g->checked_against=h->revision;
    h->staged=h->slot[slot]=g;*id=g->id;
    pthread_mutex_unlock(&runtime_lock);return 0;
}
int cnet_core_host_stage(CnetCoreHost *h,int fd,const char *name,const char *registry,uint64_t *id){
    if(id)*id=0;
    if(!h||!registry||!id)return -1;
    CnetCoreCandidate candidate;if(cnet_core_candidate_load_at(fd,name,&candidate))return -1;
    pthread_mutex_lock(&runtime_lock);collect(h);unsigned slot=0;while(slot<4&&h->slot[slot])slot++;
    if(h->staged||slot==4||h->next==UINT64_MAX){pthread_mutex_unlock(&runtime_lock);return -1;}
    Generation *g=calloc(1,sizeof *g);char error[160];
    if(g)g->core=cnet_capsule_core_open(registry,error,sizeof error);
    if(!g||!g->core){free(g);pthread_mutex_unlock(&runtime_lock);return -1;}
    g->id=h->next++;g->neural=1;g->candidate=candidate;h->staged=h->slot[slot]=g;*id=g->id;
    pthread_mutex_unlock(&runtime_lock);return 0;
}
static void append64(unsigned char **p,uint64_t value){for(unsigned i=0;i<8;i++)*(*p)++=(unsigned char)(value>>(i*8));}
int cnet_core_gate_digest(const CnetSelectorGraph *graphs,size_t n,const CnetCoreShadowCase *shadow,size_t count,char hash[65]){
    if(hash)hash[0]=0;
    if(!hash||!graphs||n<2||n>2048||!shadow||count<2||count>256)return -1;
    for(size_t i=0;i<n;i++)if(cnet_core_selector_validate(graphs+i))return -1;
    unsigned positive=0,negative=0;
    for(size_t i=0;i<count;i++){
        if(!memchr(shadow[i].request,0,128)||!shadow[i].request[0]||(shadow[i].verified!=0&&shadow[i].verified!=1)||(!shadow[i].verified&&shadow[i].expected))return -1;
        for(size_t j=0;j<i;j++)if(!strcmp(shadow[i].request,shadow[j].request))return -1;
        positive+=shadow[i].verified;negative+=!shadow[i].verified;
    }
    if(!positive||!negative)return -1;
    size_t capacity=16+n*(40+64*48)+count*144;unsigned char *bytes=calloc(1,capacity);if(!bytes)return -1;
    unsigned char *p=bytes;append64(&p,n);append64(&p,count);
    for(size_t i=0;i<n;i++){
        const CnetSelectorGraph *g=graphs+i;append64(&p,g->feature_version);append64(&p,g->n);append64(&p,g->start);append64(&p,g->goal);append64(&p,g->generation);
        for(unsigned u=0;u<g->n;u++){
            const CnetSelectorNode *v=g->node+u;append64(&p,v->identity);append64(&p,v->version);append64(&p,v->input_type);append64(&p,v->output_type);append64(&p,v->available);append64(&p,g->edge[u]);
        }
    }
    for(size_t i=0;i<count;i++){memcpy(p,shadow[i].request,strlen(shadow[i].request));p+=128;append64(&p,shadow[i].expected);append64(&p,(unsigned)shadow[i].verified);}
    int rc=cce_sha256_bytes_hex(bytes,(size_t)(p-bytes),hash);free(bytes);return rc;
}
static int reachable(const CnetSelectorGraph *g){
    unsigned char seen[64]={0};unsigned queue[64],head=0,tail=0;seen[g->start]=1;queue[tail++]=g->start;
    while(head<tail){unsigned u=queue[head++];if(u==g->goal)return 1;
        for(unsigned v=0;v<g->n;v++)if(!seen[v]&&g->node[v].available&&((g->edge[u]>>v)&1)&&g->node[u].output_type==g->node[v].input_type){seen[v]=1;queue[tail++]=v;}
    }return 0;
}
int cnet_core_host_check(CnetCoreHost *h,uint64_t id,const CnetSelectorGraph *graphs,size_t n,const CnetCoreShadowCase *shadow,size_t count,CnetCoreGateReport *report){
    if(report)memset(report,0,sizeof *report);
    if(!h||!report)return -1;
    char hash[65];int valid=cnet_core_gate_digest(graphs,n,shadow,count,hash)==0;
    pthread_mutex_lock(&runtime_lock);Generation *g=h->staged;int rc=-1;
    if(!g||g->id!=id)goto done;
    g->approved=0;
    if(!valid||strcmp(hash,g->candidate.evaluation_sha256))goto done;
    size_t pos[65]={0},neg[65]={0},completed[65]={0},abstained[65]={0};double brier[65]={0};
    for(size_t i=0;i<n;i++){
        const CnetSelectorGraph *graph=graphs+i;CnetSelectorProposal p;
        if(cnet_core_selector_propose(&g->candidate.cell,graph,64,&p))goto done;
        int expected=reachable(graph);unsigned at=graph->start;
        if(p.hops>graph->n)goto done;
        for(unsigned j=0;j<p.hops;j++){
            unsigned v=p.path[j];if(v>=graph->n||!graph->node[v].available||!((graph->edge[at]>>v)&1)||graph->node[at].output_type!=graph->node[v].input_type)goto done;at=v;
        }
        if(p.count&&(!expected||at!=graph->goal))goto done;
        unsigned size=graph->n;int complete=expected&&(p.count||graph->start==graph->goal),abstain=!expected&&p.score<.9f;
        pos[size]+=expected;neg[size]+=!expected;completed[size]+=complete;abstained[size]+=abstain;
        double error=(double)p.score-expected;brier[size]+=error*error;
        report->graphs++;report->reachable+=expected;report->unreachable+=!expected;report->completed+=complete;report->abstained+=abstain;report->brier+=error*error;
    }
    report->brier/=n;
    for(unsigned size=2;size<=64;size++)if(pos[size]||neg[size]){
        if(!pos[size]||!neg[size]||completed[size]<.95*pos[size]||abstained[size]<.95*neg[size]||brier[size]/(pos[size]+neg[size])>.01)goto done;
    }
    if(cnet_capsule_core_validate_growth_cell(h->active->core,g->core,&g->candidate.cell,g->id,&report->growth_obligations))goto done;
    for(size_t i=0;i<count;i++){
        CnetCapsuleCoreReply a,b;int ra=cnet_capsule_core_ask(g->core,shadow[i].request,&a);
        int rb=cnet_capsule_core_ask_cell(g->core,shadow[i].request,&g->candidate.cell,g->id,&b);
        if(shadow[i].verified){if(ra||rb||!a.verified||!b.verified||a.value!=shadow[i].expected||b.value!=shadow[i].expected)goto done;}
        else if(!ra||!rb||a.verified||b.verified)goto done;
        report->shadow_cases++;
    }
    if(prepare_identities(h,g))goto done;
    g->approved=1;g->checked_against=h->revision;rc=0;
done:
    pthread_mutex_unlock(&runtime_lock);return rc;
}
int cnet_core_host_activate(CnetCoreHost *h,uint64_t id){
    if(!h)return -1;
    pthread_mutex_lock(&runtime_lock);Generation *g=h->staged;int rc=-1;
    if(g&&g->id==id&&g->approved&&g->checked_against==h->revision&&h->revision<UINT64_MAX){
        free(h->known);h->known=g->known;h->known_count=g->known_count;g->known=NULL;g->known_count=0;
        h->rollback=h->active;h->active=g;h->staged=NULL;h->revision++;collect(h);rc=0;
    }
    pthread_mutex_unlock(&runtime_lock);return rc;
}
int cnet_core_host_rollback(CnetCoreHost *h){
    if(!h)return -1;
    pthread_mutex_lock(&runtime_lock);int rc=-1;
    if(h->rollback&&h->revision<UINT64_MAX){Generation *old=h->active;h->active=h->rollback;h->rollback=old;h->revision++;if(h->staged)h->staged->approved=0;rc=0;}
    pthread_mutex_unlock(&runtime_lock);return rc;
}
int cnet_core_host_discard(CnetCoreHost *h,uint64_t id){
    if(!h)return -1;
    pthread_mutex_lock(&runtime_lock);int rc=-1;
    if(h->staged&&h->staged->id==id){h->staged=NULL;collect(h);rc=0;}
    pthread_mutex_unlock(&runtime_lock);return rc;
}
