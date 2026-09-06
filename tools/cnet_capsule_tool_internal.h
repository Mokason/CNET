#ifndef CNET_CAPSULE_TOOL_INTERNAL_H
#define CNET_CAPSULE_TOOL_INTERNAL_H
int capsule_tool_integer(const char *s, unsigned *value);
int capsule_tool_eval(const char *op, unsigned operand, unsigned input, unsigned *output);
/* plan POLICY INPUT OUTPUT VALUE: complete rows on stdout only on success.
 * 0 success; 3 no approved path within the supported eight hops;
 * 2 invalid policy, overflow or work/state exhaustion.
 * Each row: INPUT OUTPUT IB OB OP OPERAND MIN MAX INPUT_VALUE OUTPUT_VALUE.
 * MIN/MAX intersect policy with the tool's representable output domain. */
int capsule_tool_plan(int argc, char **argv);
#endif
