#!/usr/bin/env python3
"""Reproducible, CPU-only structural HDC experiment. No network/model calls."""
import argparse
import ctypes as C
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import random
import re
import resource
import statistics as S
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
class Edge(C.Structure):
    _fields_ = [(x, C.c_int) for x in ('s', 'r', 'o', 'sign')]
class Answer(C.Structure):
    _fields_ = [('answer', C.c_int), ('iterations', C.c_int), ('status', C.c_int), ('us', C.c_double)]
class Factor(C.Structure):
    _fields_ = [(x, C.c_int) for x in ('x', 'y', 'z', 'iterations', 'raw_accept', 'verified_accept')] + [('us', C.c_double), ('build_us', C.c_double), ('bytes', C.c_size_t), ('reconstruction', C.c_float)]

def library():
    lib = C.CDLL(str(HERE / 'native.so'))
    lib.world_create.argtypes = [C.c_int, C.c_int, C.c_int, C.c_uint64, C.POINTER(Edge), C.c_int]
    lib.world_create.restype = C.c_void_p
    lib.world_free.argtypes = [C.c_void_p]
    lib.world_costs.argtypes = [C.c_void_p, C.POINTER(C.c_double)]
    lib.world_query.argtypes = [C.c_void_p, C.c_int, C.POINTER(C.c_int), C.c_int, C.c_int, C.c_int, C.c_int, C.POINTER(Answer)]
    lib.factor_trial.argtypes = [C.c_int, C.c_int, C.c_int, C.c_uint64, C.c_int, C.c_int, C.c_int, C.c_int, C.c_float, C.c_int, C.POINTER(Factor)]
    lib.cnet_vsa_gencap_encode_intent_q8.argtypes = [C.c_char_p, C.POINTER(C.c_int8), C.c_uint32]
    return lib

def oracle(edges, start, rels):
    negative = {(s, r, o) for s, r, o, sign in edges if sign < 0}
    reachable = {start}
    for relation in rels:
        nxt = set()
        for s, r, o, sign in edges:
            if sign > 0 and s in reachable and r == relation:
                if (s, r, o) in negative:
                    return -1
                nxt.add(o)
        reachable = nxt
    return next(iter(reachable)) if len(reachable) == 1 else -1

def parse_fact(text):
    """Deliberately finite grammar, not a learned semantic parser."""
    active = re.fullmatch(r'node(\d{3}) (not )?rel(\d{2}) node(\d{3})', text)
    if active:
        s, neg, r, o = active.groups()
        return [int(s), int(r), int(o), -1 if neg else 1]
    passive = re.fullmatch(r'node(\d{3}) is (not )?rel(\d{2}) by node(\d{3})', text)
    if passive:
        o, neg, r, s = passive.groups()
        return [int(s), int(r), int(o), -1 if neg else 1]
    raise ValueError('unsupported grammar')

def sentences(edge):
    s, r, o, sign = edge
    neg = 'not ' if sign < 0 else ''
    return [f'node{s:03d} {neg}rel{r:02d} node{o:03d}', f'node{o:03d} is {neg}rel{r:02d} by node{s:03d}']

def fixtures(seed, reps):
    rng = random.Random(seed)
    out = []
    for depth, distractors, kind, repeat in itertools.product((1, 2, 4), (8, 32, 96), ('unique', 'reversed', 'missing', 'ambiguous', 'contradictory'), range(reps)):
        n, nr = 32, 4
        nodes = rng.sample(range(n), depth + 3)
        path, alternate = nodes[:depth+1], nodes[depth+1]
        rels = [rng.randrange(nr) for _ in range(depth)]
        edges = [(path[h], rels[h], path[h+1], 1) for h in range(depth)]
        if kind == 'missing':
            del edges[rng.randrange(depth)]
        if kind == 'reversed':
            edges.append((path[1], rels[0], path[0], 1))
        if kind == 'ambiguous':
            edges.append((path[-2], rels[-1], alternate, 1))
        if kind == 'contradictory':
            h = rng.randrange(depth)
            edges.append((path[h], rels[h], path[h+1], -1))
        # Disjoint distractors control load without silently changing gold.
        pool = list(set(range(n)) - set(nodes))
        distract = set()
        while len(distract) < distractors:
            distract.add((rng.choice(pool), rng.randrange(nr), rng.choice(pool), 1))
        edges += sorted(distract)
        rng.shuffle(edges)
        gold = oracle(edges, path[0], rels)
        # Reversed edges may create additional walks; use the oracle, never a guessed label.
        out.append(dict(id=f'{seed}-{len(out):04d}', n=n, d=2048, nr=nr, depth=depth,
                        distractors=distractors, kind=kind, repeat=repeat,
                        book_seed=11 if repeat % 2 == 0 else 29,
                        start=path[0], rels=rels, edges=edges, gold=gold))
    return out

def factor_fixtures():
    out = []
    for count, noise, seed, trial in itertools.product((4, 16, 64), (0.0, 0.1), (11, 29), range(12)):
        rng = random.Random(seed * 100000 + count * 1000 + trial)
        x, y, z = [rng.randrange(count) for _ in range(3)]
        absent = trial >= 9
        for cap in (4, 16, 32):
            for lane, d in ((0, 512), (1, 512), (2, 512), (2, 2048)):
                out.append(dict(lane=lane, d=d, count=count, seed=seed, x=x, y=y, z=z,
                                absent=absent, noise=noise, cap=cap, trial=trial))
    return out

def create(lib, case):
    arr = (Edge * len(case['edges']))(*(Edge(*e) for e in case['edges']))
    world = lib.world_create(case['n'], case['d'], case['nr'], case['book_seed'], arr, len(arr))
    assert world
    return world

def query(lib, world, case, lane, cycles=8, verified=False):
    rels = (C.c_int * case['depth'])(*case['rels'])
    ans = Answer()
    lib.world_query(world, case['start'], rels, case['depth'], lane, cycles, int(verified), C.byref(ans))
    return ans

def factor_query(lib, case):
    result = Factor()
    lib.factor_trial(case['lane'], case['d'], case['count'], case['seed'], case['x'], case['y'], case['z'], int(case['absent']), case['noise'], case['cap'], C.byref(result))
    return result

def self_test(lib):
    vectors = []
    for sentence in ('Alice feeds Bob', 'Bob feeds Alice', 'node001 rel00 node002', 'node001 rel00 node003'):
        v = (C.c_int8 * 2048)()
        assert lib.cnet_vsa_gencap_encode_intent_q8(sentence.encode(), v, lib.experiment_default_encoder()) == 0
        vectors.append(bytes(v))
    assert vectors[0] == vectors[1], 'baseline role collision changed; review protocol'
    assert vectors[2] != vectors[3], 'entity strings collapse in production encoder'
    print('HDC_ROLE_COLLISION_RED: reversed roles have identical production int8 vectors', flush=True)
    for e in ([1, 0, 2, 1], [2, 3, 1, -1]):
        for s in sentences(e):
            assert parse_fact(s) == e
    try:
        parse_fact('perhaps node001 knows node002')
        raise AssertionError('unsupported grammar accepted')
    except ValueError:
        pass
    # Disjoint development seed; only implementation sanity, no fitting.
    for case in fixtures(1701, 1):
        world = create(lib, case)
        try:
            assert query(lib, world, case, 0).answer == case['gold']
        finally:
            lib.world_free(world)
    for lane, d in ((0, 512), (1, 512), (2, 512), (2, 2048)):
        case = dict(lane=lane, d=d, count=1, seed=71, x=0, y=0, z=0, absent=False, noise=0, cap=4)
        r = factor_query(lib, case)
        assert (r.x, r.y, r.z) == (0, 0, 0)
        assert r.raw_accept and r.verified_accept, (case, r.reconstruction)
    print('HARNESS_SELF_TEST_PASS: parser, independent oracle, singleton factor recovery', flush=True)
    return {'production_role_vectors_identical': True, 'production_entity_vectors_distinct': True,
            'development_oracle_cases': 45, 'singleton_factor_cases': 4}

def percentile(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs)-1, int((len(xs)-1)*q))]

def rel_summary(rows):
    n = len(rows)
    valid = sum(r['gold'] >= 0 for r in rows)
    invalid = n-valid
    right = sum(r['answer'] >= 0 and r['answer'] == r['gold'] for r in rows)
    wrong = sum(r['answer'] >= 0 and r['answer'] != r['gold'] for r in rows)
    refuse = sum(r['answer'] < 0 and r['gold'] < 0 for r in rows)
    return dict(n=n, valid=valid, invalid=invalid, correct_answers=right, wrong_answers=wrong,
                correct_refusals=refuse, total_accuracy=(right+refuse)/n,
                valid_answer_recall=right/valid if valid else None,
                wrong_among_accepted=wrong/(right+wrong) if right+wrong else None,
                invalid_refusal=refuse/invalid if invalid else None,
                median_us=S.median(r['us'] for r in rows), p95_us=percentile([r['us'] for r in rows], .95))

def factor_summary(rows):
    valid = [r for r in rows if not r['absent']]
    invalid = [r for r in rows if r['absent']]
    exact = lambda r: (r['rx'], r['ry'], r['rz']) == (r['x'], r['y'], r['z']) and not r['absent']
    return dict(n=len(rows), valid=len(valid), invalid=len(invalid), exact_ids=sum(exact(r) for r in valid)/len(valid),
                raw_correct_accept=sum(exact(r) and r['raw_accept'] for r in valid)/len(valid),
                verified_correct_accept=sum(exact(r) and r['verified_accept'] for r in valid)/len(valid),
                raw_wrong_accept=sum(r['raw_accept'] and not exact(r) for r in rows),
                verified_wrong_accept=sum(r['verified_accept'] and not exact(r) for r in rows),
                absent_false_accept=sum(r['verified_accept'] for r in invalid),
                no_accept=sum(not r['raw_accept'] for r in rows),
                valid_median_us=S.median(r['us'] for r in valid),
                median_us=S.median(r['us'] for r in rows), p95_us=percentile([r['us'] for r in rows], .95),
                median_iterations=S.median(r['iterations'] for r in rows), logical_book_bytes=rows[0]['bytes'])

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--out', type=Path, default=ROOT / 'result' / 'cnet_hdc_structure_20260911')
    args = parser.parse_args()
    available = os.sched_getaffinity(0)
    os.sched_setaffinity(0, {min(available)})
    lib = library()
    checks = self_test(lib)
    if args.self_test:
        return
    rel_cases, factor_cases = fixtures(20260911, 8), factor_fixtures()
    manifest = {'protocol': 1, 'relational': rel_cases, 'factorization': factor_cases}
    encoded = json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()
    digest = hashlib.sha256(encoded).hexdigest()
    # Persist the fixed test set BEFORE any scored execution.
    args.out.with_suffix('.fixtures.json').write_bytes(encoded + b'\n')
    print(f'FIXTURES_FROZEN sha256={digest} relational={len(rel_cases)} factor={len(factor_cases)}', flush=True)
    rows, costs, parse_times = [], [], []
    lanes = [(0, 0, False, 'exact'), (1, 0, False, 'topical'), (2, 0, False, 'direct'),
             (2, 0, True, 'direct_verified'), (3, 8, False, 'coupled8'), (3, 8, True, 'coupled8_verified')]
    started = time.monotonic()
    for i, case in enumerate(rel_cases):
        texts = [s for e in case['edges'] for s in sentences(e)]
        t = time.perf_counter_ns()
        parsed = [parse_fact(s) for s in texts]
        parse_times.append((time.perf_counter_ns()-t)/1000)
        assert parsed == [list(e) for e in case['edges'] for _ in range(2)]
        world = create(lib, case)
        try:
            cost = (C.c_double * 6)()
            lib.world_costs(world, cost)
            costs.append(dict(id=case['id'], facts=len(case['edges']), encode_us=cost[0], topical_build_us=cost[1],
                              structural_bytes=int(cost[2]), registry_allocated_bytes=int(cost[3]),
                              raw_facts_bytes=int(cost[4]), world_metadata_bytes=int(cost[5])))
            order = lanes.copy()
            # Alternate order to reduce fixed thermal/cache bias.
            random.Random(i).shuffle(order)
            for lane, cycles, verified, name in order:
                query(lib, world, case, lane, cycles, verified)  # warm-up
                samples = [query(lib, world, case, lane, cycles, verified) for _ in range(3)]
                assert len({a.answer for a in samples}) == 1
                ans = samples[0]
                if lane == 0:
                    assert ans.answer == case['gold'], case['id']
                rows.append({k: case[k] for k in ('id', 'depth', 'distractors', 'kind', 'gold')} |
                            dict(lane=name, answer=ans.answer, iterations=ans.iterations, us=S.median(a.us for a in samples)))
            # Fixed-cycle propagation probe on unique-answer fixtures.
            if case['kind'] == 'unique':
                for cycles in (1, 2, 4):
                    a = query(lib, world, case, 3, cycles)
                    rows.append({k: case[k] for k in ('id', 'depth', 'distractors', 'kind', 'gold')} |
                                dict(lane=f'coupled{cycles}', answer=a.answer, iterations=a.iterations, us=a.us))
        finally:
            lib.world_free(world)
        if i % 60 == 0:
            print(f'RELATIONAL {i+1}/{len(rel_cases)} elapsed={time.monotonic()-started:.1f}s', flush=True)
    frows = []
    for i, case in enumerate(factor_cases):
        # 3 repeated native trials; books generated outside measured solve time.
        samples = [factor_query(lib, case) for _ in range(3)]
        r = samples[0]
        assert len({(a.x,a.y,a.z,a.raw_accept,a.iterations) for a in samples}) == 1
        frows.append(case | dict(rx=r.x, ry=r.y, rz=r.z, iterations=r.iterations,
                                raw_accept=bool(r.raw_accept), verified_accept=bool(r.verified_accept),
                                reconstruction=r.reconstruction, bytes=r.bytes,
                                us=S.median(a.us for a in samples), build_us=S.median(a.build_us for a in samples)))
        if i % 240 == 0:
            print(f'FACTORIZATION {i+1}/{len(factor_cases)} elapsed={time.monotonic()-started:.1f}s', flush=True)
    summary = {name: rel_summary([r for r in rows if r['lane'] == name]) for name in sorted({r['lane'] for r in rows})}
    by_depth = {f'{name}/depth{depth}': rel_summary([r for r in rows if r['lane'] == name and r['depth'] == depth])
                for name in summary for depth in (1,2,4)}
    fsummary = {}
    for lane, d, count, noise, cap in sorted({(r['lane'],r['d'],r['count'],r['noise'],r['cap']) for r in frows}):
        group = [r for r in frows if (r['lane'],r['d'],r['count'],r['noise'],r['cap']) == (lane,d,count,noise,cap)]
        fsummary[f'lane{lane}/d{d}/m{count}/noise{noise}/cap{cap}'] = factor_summary(group)
    metadata = dict(fixture_sha256=digest, checks=checks, cpu_affinity=sorted(os.sched_getaffinity(0)),
                    platform=platform.platform(), python=platform.python_version(),
                    native_sha256=hashlib.sha256((HERE/'native.so').read_bytes()).hexdigest(),
                    source_sha256={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in
                                   [HERE/'native.c', HERE/'run.py', ROOT/'src/cnet_vsa_reason.c', ROOT/'src/cnet_vsa_text.c', ROOT/'src/cnet_vsa_gen_capsule.c']},
                    compiler=subprocess.check_output(['cc','--version'],text=True).splitlines()[0],
                    process_peak_rss_kib=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
                    wall_seconds=time.monotonic()-started, controlled_parser_sentences=sum(2*len(c['edges']) for c in rel_cases),
                    parser_median_fixture_us=S.median(parse_times), transformer_comparison='WITHHELD: no scored model calls')
    result = dict(metadata=metadata, relational_summary=summary, relational_by_depth=by_depth,
                  factor_summary=fsummary, relational_rows=rows, factor_rows=frows, world_costs=costs)
    args.out.with_suffix('.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({'metadata': metadata, 'relational_summary':summary},indent=2), flush=True)
    print(f'RESULT_WRITTEN {args.out.with_suffix(".json")}', flush=True)

if __name__ == '__main__':
    main()
