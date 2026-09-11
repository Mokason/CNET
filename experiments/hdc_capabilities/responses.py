"""Compose a fixed-format answer only after checking explicit signed evidence."""
from procedures import execute

def compose(edges, start, program, proposed):
    answer = execute(edges, start, program)
    if answer is None or answer != proposed:
        return 'ABSTAIN: no uniquely supported answer.'
    paths = {start: ()}
    for relation in program:
        nxt = {}
        for s, r, o, sign in edges:
            if sign > 0 and r == relation and s in paths and o not in nxt:
                nxt[o] = paths[s] + ((s,r,o,sign),)
        paths = nxt
    if answer not in paths:
        return 'ABSTAIN: evidence path unavailable.'
    evidence = ' '.join(f'node{s:03d} rel{r:02d} node{o:03d}.' for s,r,o,_ in paths[answer])
    return f'node{answer:03d}. {evidence}'

if __name__ == '__main__':
    facts = [(0,0,1,1),(1,1,2,1)]
    assert compose(facts,0,(0,1),2) == 'node002. node000 rel00 node001. node001 rel01 node002.'
    assert compose(facts,0,(0,1),1).startswith('ABSTAIN:')
    assert compose(facts+[(1,1,2,-1)],0,(0,1),2).startswith('ABSTAIN:')
    print('RESPONSE_SANITY_PASS')
