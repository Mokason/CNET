"""Follow-up to reload RSS growth: native cache only, no retained result graphs."""
import json
import subprocess
import sys
from branching import Cache, Spec
from run import rss
from scaling_run import CountedNative, OUT


def worker(regime):
    native = CountedNative()
    specs = [Spec(**s) for s in json.loads((OUT/'specs.json').read_text())]
    cache = Cache(native, 4 if regime == 'lru4' else 32)
    samples = [dict(cycles=0, rss=rss())]
    for cycle in range(1024):
        if regime == 'reload_all':
            cache.close()
        for spec in specs:
            with cache.pin(spec):
                value, hops, _ = cache.ask(spec, cycle % 64)
                i = int(spec.key)
                assert value == ((2*(i % 8)+1)*(cycle % 64)+(3*i+1)) % 64 and hops > 0
        if cycle % 64 == 63:
            samples.append(dict(cycles=cycle+1, rss=rss()))
    cache.close()
    assert native.live == 0
    print(json.dumps(dict(regime=regime, cycles=1024, native_calls=native.calls,
                         live_core_peak=native.peak, loads=cache.loads, rss_samples=samples,
                         rss_after_close=rss(), live_cores_after_close=native.live)))


def main():
    if len(sys.argv) > 1:
        worker(sys.argv[1])
        return
    rows = []
    for regime in ('all_resident', 'lru4', 'reload_all'):
        output = subprocess.check_output([sys.executable, __file__, regime], text=True)
        row = json.loads(output)
        rows.append(row)
        (OUT/'memory_followup.json').write_text(json.dumps(rows, indent=2)+'\n')
        print(regime, row['rss_samples'], flush=True)


if __name__ == '__main__':
    main()
