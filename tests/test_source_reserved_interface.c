/* A schema-1 unit cannot impersonate an evidence-bearing source interface. */
#define main original_capsule_fixture_main
#include "test_knowledge_capsule.c"
#undef main
#include "cnet_capsule_core.h"

int main(void) {
    char root[]="/tmp/cnet-source-reserved-XXXXXX",unit[256],error[256];
    if(!mkdtemp(root))return 2;
    snprintf(unit,sizeof unit,"%s/unbound",root);
    CnetBase b;HybridAi h;CnetCapsuleReport report;cnb_init(&b);hybrid_ai_init(&h);
    check(!build_unit_ports(&b,&h,"unbound",P("cnet_source_fact"),P("unbound_numeric"))&&
          !cnet_capsule_export(&b,&h,"unbound",unit,&report),"independently valid numeric capsule fixture exports");
    CnetCapsuleCore *c=cnet_capsule_core_open(root,error,sizeof error);
    check(!c,"SOURCE_RESERVED_RED source input cannot bypass evidence through a numeric route");
    cnet_capsule_core_close(c);cnb_free(&b);hybrid_ai_free(&h);
    printf("SOURCE_RESERVED_%s checks=%d failures=%d\n",failures?"RED":"PASS",checks,failures);
    rm_pack(unit);rmdir(root);return failures?1:0;
}
