"""Scaling campaign: real capsules, exact oracle, isolated timing workers."""
import ctypes as ct
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import resource
import subprocess
import sys
import time

from branching import Cache, Context, Native, Refusal, Spec
from run import rss, stats
from scaling import Node, execute, verify

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'var/capsule_scaling_20260913'
CONTEXT = Context('numeric_fixture', 'mod64', 'actual')


class CountedNative(Native):
    def __init__(self):
        super().__init__(ROOT / 'bin/libcnet_capsule_core.so')
        self.live = self.peak = self.calls = 0

    def open(self, spec):
        handle = super().open(spec)
        self.live += 1
        self.peak = max(self.peak, self.live)
        return handle

    def close(self, handle):
        super().close(handle)
        self.live -= 1

    def ask(self, handle, spec, value):
        self.calls += 1
        return super().ask(handle, spec, value)


def prepare():
    native = Native(ROOT / 'bin/libcnet_capsule_core.so')
    specs, receipts = [], []
    for i in range(32):
        root = OUT / f'capsule_{i:02}'
        root.mkdir(exist_ok=True, mode=0o700)
        source = OUT / f'labels_{i:02}.tsv'
        rows = subprocess.check_output([sys.executable, str(Path(__file__).with_name('scaling_oracle.py')), str(i)])
        source.write_bytes(rows)
        argv = [str(ROOT/'bin/cnet_capsule_core'), 'teach', str(root), f'scale_{i:02}',
                'state6', 'next6', '6', '6', 'verified_tool', str(source)]
        call = subprocess.run(argv, capture_output=True, text=True, check=True, timeout=60)
        (OUT/f'build_{i:02}.log').write_text(call.stdout+call.stderr)
        assert 'CAPSULE_TEACH_PASS' in call.stdout and 'method=finite_domain_compile' in call.stdout
        error = ct.create_string_buffer(160)
        handle = native.lib.cnet_capsule_core_open(str(root).encode(), error, len(error))
        if not handle:
            raise RuntimeError(error.value)
        try:
            identity = native.identity(handle)
        finally:
            native.close(handle)
        specs.append(dict(key=str(i), root=str(root), input_tag='state6', output_tag='next6', identity=identity))
        receipts.append(dict(argv=argv, identity=identity, rows_sha256=hashlib.sha256(rows).hexdigest()))
    assert len({s['identity'] for s in specs}) == 32
    (OUT/'specs.json').write_text(json.dumps(specs, indent=2)+'\n')
    (OUT/'build_receipts.json').write_text(json.dumps(receipts, indent=2)+'\n')


def plan(shape, size, specs, inputs):
    if shape == 'wide':
        return [Node(i, specs[i], inputs[i], ()) for i in range(size)], tuple(range(size))
    if shape == 'chain':
        return [Node(i, specs[i], inputs[0] if i == 0 else None, () if i == 0 else (i-1,))
                for i in range(size)], (size-1,)
    nodes = [Node(i, specs[i], inputs[i], ()) for i in range(size)]
    level = list(range(size))
    while len(level) > 1:
        following = []
        for i in range(0, len(level), 2):
            index = len(nodes)
            nodes.append(Node(index, specs[index], None, tuple(level[i:i+2])))
            following.append(index)
        level = following
    return nodes, tuple(level)


def reference(nodes, outputs):
    values = {}
    for node in nodes:
        x = sum(values[p] for p in node.parents) % 64 if node.parents else node.literal
        i = int(node.spec.key)
        values[node.index] = ((2*(i % 8)+1)*x+(3*i+1)) % 64
    return sum(values[i] for i in outputs), values


def worker(shape, size, regime):
    specs = [Spec(**s) for s in json.loads((OUT/'specs.json').read_text())]
    count = size*2-1 if shape == 'tree' else size
    native = CountedNative()
    capacity = min(4, count) if regime == 'lru4' else count
    cache = Cache(native, capacity)
    before = rss()
    for spec in specs[:count]:
        with cache.pin(spec):
            pass
    resident = rss()
    loads_before = cache.loads
    rng = random.Random(20260913+size)
    times, cpus, errors = [], [], 0
    digest = hashlib.sha256()
    for trial in range(256):
        inputs = [trial % 64] + [rng.randrange(64) for _ in range(size-1)]
        if regime == 'reload_all':
            cache.close()
        start, cpu_start = time.perf_counter_ns(), time.thread_time_ns()
        nodes, outputs = plan(shape, size, specs, inputs)
        result = execute(cache, nodes, outputs, f'{shape}-{size}-{trial}', CONTEXT)
        rendered = f'value={result.value}; sources='+','.join(r.node.spec.identity for r in result.receipts)
        cpus.append((time.thread_time_ns()-cpu_start)/1000)
        times.append((time.perf_counter_ns()-start)/1000)
        expected, intermediate = reference(nodes, outputs)
        errors += int(result.value != expected or any(r.value != intermediate[r.node.index] for r in result.receipts))
        assert rendered
        digest.update(str((inputs, result.value)).encode())
    after = rss()
    cache.close()
    verify(result)
    assert native.live == 0 and native.peak <= capacity and errors == 0
    result = dict(shape=shape, size=size, capsule_count=count, regime=regime, cases=256,
                  wrong=errors, latency=stats(times), mean_thread_cpu_us=sum(cpus)/len(cpus),
                  live_core_peak=native.peak, capacity=capacity, native_calls=native.calls,
                  loads_during_measurement=cache.loads-loads_before, evictions=cache.evictions,
                  rss_before=before, rss_resident=resident, rss_after_work=after, rss_after_close=rss(),
                  ru_maxrss_bytes=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss*1024,
                  output_sha256=digest.hexdigest())
    result['target_p95_1ms'] = result['latency']['p95_us'] <= 1000
    result['ceiling_p99_5ms'] = result['latency']['p99_us'] <= 5000
    print(json.dumps(result))


def combinations():
    specs = [Spec(**s) for s in json.loads((OUT/'specs.json').read_text())][:2]
    cache = Cache(CountedNative(), 2)
    rows = []
    for depth in range(1, 9):
        signatures, cases = set(), 0
        for choices in itertools.product(range(2), repeat=depth):
            signature = []
            for x in range(64):
                nodes = [Node(i, specs[c], x if i == 0 else None, () if i == 0 else (i-1,))
                         for i, c in enumerate(choices)]
                result = execute(cache, nodes, (depth-1,), f'combo-{depth}-{choices}-{x}', CONTEXT)
                expected, values = reference(nodes, (depth-1,))
                assert result.value == expected and all(r.value == values[r.node.index] for r in result.receipts)
                signature.append(result.value)
                cases += 1
            signatures.add(tuple(signature))
        rows.append(dict(depth=depth, distinct_modules=2, sequences=2**depth,
                         distinct_input_output_functions=len(signatures), exhaustive_cases=cases, wrong=0))
    cache.close()
    return rows


def main():
    if len(sys.argv) > 1 and sys.argv[1] == 'worker':
        worker(sys.argv[2], int(sys.argv[3]), sys.argv[4])
        return
    OUT.mkdir(parents=True, exist_ok=True)
    protocol = Path(__file__).with_name('scaling_protocol.json').read_text()
    if (OUT/'protocol.json').exists():
        assert json.loads((OUT/'protocol.json').read_text()) == json.loads(protocol)
    else:
        (OUT/'protocol.json').write_text(protocol)
    if not (OUT/'specs.json').exists():
        prepare()
    cells = []
    for shape in ('wide', 'chain', 'tree'):
        for size in ((1, 2, 4, 8, 16) if shape == 'tree' else (1, 2, 4, 8, 16, 32)):
            for regime in ('all_resident', 'lru4', 'reload_all'):
                call = subprocess.run([sys.executable, __file__, 'worker', shape, str(size), regime],
                                      capture_output=True, text=True, timeout=90, check=True)
                cell = json.loads(call.stdout)
                cells.append(cell)
                print(shape, size, regime, 'p99_us', round(cell['latency']['p99_us'], 2), flush=True)
                (OUT/'cells.json').write_text(json.dumps(cells, indent=2)+'\n')
    report = dict(cells=cells, combinations=combinations(), protocol=json.loads((OUT/'protocol.json').read_text()),
                  scope='synthetic exact finite-domain capsule graphs; experimental checked adapters; no prose planner',
                  timing='fresh process per cell; includes plan construction, native calls, receipts, joins and rendering; excludes process startup and oracle; warm filesystem')
    (OUT/'numeric_report.json').write_text(json.dumps(report, indent=2)+'\n')
    print('SCALING_NUMERIC_PASS', sum(c['cases'] for c in cells), 'graph cases', flush=True)


if __name__ == '__main__':
    main()
