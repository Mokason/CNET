# CORE brick seeds

The `.lut` files here are finite factory table seeds, not GGUF weights
or portable CNU1 capsules. Their historical scenario naming is not a broad
capability claim.

`CNET_CORE_BUS_BRICKS_DIR` selects the runtime bank.
[scripts/cnet_core_bricks_wire.sh](../../scripts/cnet_core_bricks_wire.sh)
can install/synchronize seeds; it changes runtime state. Inspect its selected
destination and preserve local additions before intentional use.
Do not combine installation and a service restart into a routine smoke test.

Table lookup still needs its own identity/acceptance rules.
Residual generation cannot certify a table or invent a covered result.
See [dispatch](../../docs/dispatch.md), [daemon](../../docs/CNETD.md) and
[operations](../../docs/CNET_MARBLE_24_7.md).
