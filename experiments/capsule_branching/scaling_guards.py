"""Native failure at an intermediate dependency must stop later execution."""
from contextlib import ExitStack
import ctypes as ct
from dataclasses import replace
import hashlib
import json
import subprocess
import sys

from branching import Cache, Refusal, Spec
from scaling import execute
from scaling_run import CONTEXT, CountedNative, OUT, ROOT, plan, reference


def main():
    native = CountedNative()
    specs = [Spec(**s) for s in json.loads((OUT/'specs.json').read_text())]
    root = OUT/'partial'
    root.mkdir(exist_ok=True, mode=0o700)
    rows = subprocess.check_output([sys.executable, str(ROOT/'experiments/capsule_branching/scaling_oracle.py'), '0'])
    source = OUT/'partial.tsv'
    source.write_bytes(b'\n'.join(rows.splitlines()[:32])+b'\n')
    argv = [str(ROOT/'bin/cnet_capsule_core'), 'teach', str(root), 'scale_partial',
            'state6', 'next6', '6', '6', 'verified_tool', str(source)]
    built = subprocess.run(argv, capture_output=True, text=True, check=True)
    (OUT/'partial_build.log').write_text(built.stdout+built.stderr)
    assert 'CAPSULE_TEACH_PASS' in built.stdout
    error = ct.create_string_buffer(160)
    handle = native.lib.cnet_capsule_core_open(str(root).encode(), error, len(error))
    if not handle:
        raise RuntimeError(error.value)
    try:
        identity = native.identity(handle)
    finally:
        native.lib.cnet_capsule_core_close(handle)
    partial = Spec('0', str(root), 'state6', 'next6', identity)
    paths = sorted(OUT.glob('capsule_*/**/unit.cnb'))+sorted(OUT.glob('capsule_*/**/manifest.cknow'))
    hashes = {str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    checks = []
    for size in (2,4,8,16,32):
        middle = size//2
        for x in range(64):
            nodes, outputs = plan('chain', size, specs, [x])
            _, values = reference(nodes, outputs)
            if values[middle-1] >= 32:
                break
        nodes[middle] = replace(nodes[middle], spec=partial)
        cache = Cache(native, 4)
        before = native.calls
        try:
            execute(cache, nodes, outputs, f'coverage-{size}', CONTEXT)
        except Refusal:
            pass
        else:
            raise AssertionError('uncovered intermediate branch answered')
        assert native.calls-before == middle+1
        cache.close()
        assert native.live == 0
        checks.append(dict(size=size, refused_node=middle, native_calls=middle+1,
                           later_nodes_executed=0, input_to_refused_node=values[middle-1]))
    cache = Cache(native, 32)
    with ExitStack() as stack:
        for spec in specs:
            stack.enter_context(cache.pin(spec))
        before = cache.loads
        try:
            with cache.pin(partial):
                pass
        except Refusal:
            pass
        else:
            raise AssertionError('admission evicted an active capsule')
        assert native.live == 32 and cache.loads == before
    cache.close()
    assert native.live == 0
    for path, digest in hashes.items():
        assert hashlib.sha256((ROOT/path).read_bytes()).hexdigest() == digest
    report = dict(intermediate_coverage=checks, all_32_pinned_admission='PASS',
                  capsule_files_unchanged='PASS', files_sha256=hashes,
                  partial_identity=identity, partial_build_argv=argv,
                  partial_rows_sha256=hashlib.sha256(source.read_bytes()).hexdigest())
    (OUT/'guards.json').write_text(json.dumps(report, indent=2)+'\n')
    print('SCALING_NATIVE_GUARDS_PASS')


if __name__ == '__main__':
    main()
