# Live miss → table → brick → answer

Status: **IMPLEMENTED**

## Flow

1. `teach TAG n m` / `TAG n = m` → typed miss with out  
2. Domain reaches **16/16** → sync evolve admits → `.lut`  
3. Retry serve on same ask  
4. Later `TAG n` → LOCAL CERT  
5. `goal: prove TAG at n` → sync evolve + CERT answer when possible  

## Gate

```bash
make cnet_live_miss_loop
# CNET_LIVE_MISS_LOOP_PASS
```

## Law

No residual auto-CERT. Query-only misses wait for taught outs.
