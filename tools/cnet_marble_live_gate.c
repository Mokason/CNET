/* Gate main for the Marble Live wiring module.
 * make marble_live -> MARBLE_LIVE_WIRING_PASS -> MARBLE_LIVE_PASS */
#include "cnet_marble_live.h"
int main(void) { return cnet_ml_selftest(); }
