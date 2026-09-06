#ifndef CNET_CELL_CAPSULE_H
#define CNET_CELL_CAPSULE_H
#include "cnet_core_cell.h"
#include "nn.h"
/* Exact scalar FP32 -> double conversion, without training or certification.
 * Caller supplies an empty output BTN and owns it on success (btn_free).
 * Only the cell's 3-bit -> 1-bit binary-MSB layout is supported. A converted
 * model MUST still pass existing contract, coverage and capsule gates. */
int cnet_core_cell_to_btn(const CnetCoreCell *cell,Port input,Port output,
                          BinaryTransformNetwork *btn);
#endif
