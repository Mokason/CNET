#ifndef CNET_SELECTOR_FIXTURE_H
#define CNET_SELECTOR_FIXTURE_H
#include "cnet_core_selector.h"
#define SELECTOR_FAMILIES 5
#define SELECTOR_EVAL_CASES 1280
typedef struct {unsigned family,index;CnetSelectorGraph graph;} SelectorFixture;
uint32_t selector_random(uint32_t *state);
void selector_generate(CnetSelectorGraph *g,unsigned family,unsigned n,uint32_t seed,int reachable);
void selector_stress_generate(CnetSelectorGraph *g,unsigned family,unsigned n,uint32_t seed,int reachable);
void selector_permute(CnetSelectorGraph *out,const CnetSelectorGraph *in,uint32_t seed);
void selector_teacher(const CnetSelectorGraph *g,int distance[64]);
const char *selector_family(unsigned family);
#endif
