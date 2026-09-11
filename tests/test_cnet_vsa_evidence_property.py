#!/usr/bin/env python3
"""Disjoint seeded graph differential check against an integer-bitset oracle."""
import ctypes as C
from pathlib import Path
import random
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
class Fact(C.Structure):
    _fields_=[(name,C.c_int) for name in ('subject','relation','object','sign')]

def gold(edges,start,path):
    positive={};negative=set()
    for s,r,o,sign in edges:
        if sign<0:negative.add((s,r,o))
        else:positive[(s,r)]=positive.get((s,r),0)|(1<<o)
    state=1<<start
    for r in path:
        nxt=0
        while state:
            source=(state&-state).bit_length()-1;state&=state-1
            targets=positive.get((source,r),0)
            for a,b,c in negative:
                if a==source and b==r and targets&(1<<c):return None
            nxt|=targets
        state=nxt
    return state.bit_length()-1 if state and state&(state-1)==0 else None

with tempfile.TemporaryDirectory(prefix='cnet-evidence-property-') as td:
    so=Path(td)/'evidence.so'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC',
                    '-I'+str(ROOT/'include'),str(ROOT/'src/cnet_vsa_evidence.c'),'-o',str(so)],check=True)
    lib=C.CDLL(str(so));fn=lib.cnet_vsa_evidence_response
    fn.argtypes=[C.POINTER(Fact),C.c_size_t,C.c_int,C.POINTER(C.c_int),C.c_size_t,C.c_int,C.c_char_p,C.c_size_t]
    rng=random.Random(20260913);checks=0;answered=0
    for case in range(2048):
        n=rng.choice((4,8,16,32,128));hops=rng.randrange(1,9);start=rng.randrange(n)
        path=[rng.randrange(8) for _ in range(hops)]
        edges=[(rng.randrange(n),rng.randrange(8),rng.randrange(n),-1 if rng.random()<.15 else 1)
               for _ in range(rng.randrange(0,500))]
        # Include valid reachable chains in otherwise arbitrary, possibly cyclic graphs.
        at=start
        if case%2:
            for r in path:
                target=rng.randrange(n);edges.append((at,r,target,1));at=target
        expected=gold(edges,start,path)
        arr=(Fact*len(edges))(*(Fact(*e) for e in edges));rels=(C.c_int*hops)(*path)
        for proposal in (-1, rng.randrange(n)):
            out=C.create_string_buffer(512);rc=fn(arr,len(arr),start,rels,hops,proposal,out,len(out));text=out.value.decode();checks+=1
            accepted=expected is not None and (proposal==-1 or proposal==expected)
            assert (rc==0)==accepted,(case,proposal,expected,rc,text)
            if accepted:
                answered+=1
                assert text.startswith(f'node{expected:03d}. ')
                triples=[tuple(map(int,e)) for e in re.findall(r'node(\d{3}) rel(\d{2}) node(\d{3})\.',text)]
                assert len(triples)==hops
                at=start
                for (s,r,o),want in zip(triples,path):
                    assert s==at and r==want and (s,r,o,1) in edges and (s,r,o,-1) not in edges
                    at=o
                assert at==expected
            else:assert rc==1 and text.startswith('ABSTAIN:')
    print(f'CNET_VSA_EVIDENCE_PROPERTY_PASS graphs=2048 checks={checks} accepted_proofs={answered}')
