#ifndef CONTROLLER_FIXTURE_H
#define CONTROLLER_FIXTURE_H
#include <stdint.h>
#define NODES 8
#define INPUTS 144 /* adjacency + compatibility + current + goal */
#define ACTIONS 9  /* nodes 0..7; 8 means abstain */
typedef struct { uint8_t edge[64], compatible[64]; int start, goal; } Task;
uint32_t next_random(uint32_t *state);
void task_generate(Task *task, uint32_t *state);
void task_features(const Task *task, int current, float out[INPUTS]);
/* Uniform distribution over all shortest next hops, or abstain; returns distance. */
int task_teacher(const Task *task, int current, float out[ACTIONS]);
/* Independent checker: 0 rejects, 1 valid intermediate, 2 verified arrival. */
int task_check(const Task *task, int current, int proposed);
uint64_t task_topology(const Task *task);
#endif
