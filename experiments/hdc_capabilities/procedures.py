"""Bounded relation-program induction from independently verified examples."""
from itertools import product

PROGRAMS = tuple(p for depth in (1, 2, 3) for p in product(range(4), repeat=depth))

def execute(edges, start, program):
    negative = {(s, r, o) for s, r, o, sign in edges if sign < 0}
    active = {start}
    for relation in program:
        nxt = set()
        for s, r, o, sign in edges:
            if sign > 0 and s in active and r == relation:
                if (s, r, o) in negative:
                    return None
                nxt.add(o)
        active = nxt
    return next(iter(active)) if len(active) == 1 else None

def key(edges, start):
    return tuple(sorted(tuple(e) for e in edges)), start

def memorise(demonstrations):
    memory = {}
    for demo in demonstrations:
        k = key(demo['edges'], demo['start'])
        if k in memory and memory[k] != demo['answer']:
            return None
        memory[k] = demo['answer']
    return memory

def lookup(memory, edges, start):
    return memory.get(key(edges, start)) if memory is not None else None

def learn(demonstrations):
    # Keep the entire version space; never select an arbitrary first match.
    if not demonstrations:
        return ()
    return tuple(p for p in PROGRAMS if all(
        execute(d['edges'], d['start'], p) == d['answer'] for d in demonstrations))

def predict(programs, edges, start):
    if not programs:
        return None
    first = execute(edges, start, programs[0])
    if first is None:
        return None
    for program in programs[1:]:
        if execute(edges, start, program) != first:
            return None
    return first

if __name__ == '__main__':
    demos = [{'edges': [(0,0,1,1),(1,1,2,1)], 'start': 0, 'answer': 2}]
    p = learn(demos)
    assert p == ((0,1),)
    new = [(3,0,4,1),(4,1,5,1)]
    assert lookup(memorise(demos),new,3) is None
    print('PROCEDURE_LOOKUP_RED: stored examples do not transfer to new graph')
    assert predict(p,new,3) == 5
    assert learn(demos + [dict(demos[0],answer=1)]) == ()
    assert predict(((0,),(1,)),[(0,0,1,1),(0,1,2,1)],0) is None
    print('PROCEDURE_SANITY_PASS')
