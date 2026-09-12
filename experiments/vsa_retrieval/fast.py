"""Native stem-keyed sparse query path; model vocabulary compilation is offline."""
import ctypes as ct
import hashlib
import subprocess
from pathlib import Path
import numpy as np
from retrieval import CACHE, Native

class SparseView(ct.Structure):
    _fields_=[('keys',ct.c_void_p),('offset',ct.c_void_p),('owners',ct.c_void_p),
              ('weights',ct.c_void_p),('nkeys',ct.c_int),('nc',ct.c_int)]

class FastKernel:
    def __init__(self,lexical,semantic):
        from runtime import unpack_sparse
        if lexical.nc!=semantic.nc or not 1<=lexical.nc<=4096:
            raise ValueError('invalid capsule count')
        # Reuse the checked deserializer to protect all pointers crossing into C.
        self.indexes=[unpack_sparse({k:getattr(s,k) for k in ('keys','offset','owners','weights')},'',s.nc) for s in (lexical,semantic)]
        self.views=[SparseView(*(getattr(s,k).ctypes.data for k in ('keys','offset','owners','weights')),len(s.keys),s.nc) for s in self.indexes]
        native=Native()
        source=Path(__file__).with_suffix('.c')
        digest=hashlib.sha256(source.read_bytes()+native.lib._name.encode()).hexdigest()[:16]
        lib=CACHE/f'fast-{digest}.so'
        if not lib.exists():
            subprocess.run(['gcc','-std=c11','-O3','-march=native','-Wall','-Wextra','-Werror','-fPIC','-shared',str(source),native.lib._name,'-o',str(lib)],check=True)
        self.lib=ct.CDLL(str(lib))
        self.lib.fast_scores.argtypes=[ct.c_char_p,ct.POINTER(SparseView),ct.POINTER(SparseView),ct.c_int,ct.c_void_p]
        self.lib.fast_top.argtypes=[ct.c_void_p,ct.c_int,ct.c_int,ct.c_void_p]
        self.lib.fast_query.argtypes=[ct.c_char_p,ct.POINTER(SparseView),ct.POINTER(SparseView),ct.c_int,ct.c_int,ct.c_void_p,ct.c_void_p]
        self.nc=lexical.nc
        self.refs=[ct.byref(v) for v in self.views]

    def scores(self,text,mode=1):
        if not isinstance(text,str) or '\0' in text:
            raise ValueError('query must be text without NUL')
        raw=text.encode()
        if len(raw)>4096 or mode not in (0,1):raise ValueError('invalid query length or mode')
        out=np.empty(self.nc,np.float32)
        if self.lib.fast_scores(raw,*self.refs,mode,out.ctypes.data)<0:
            raise ValueError('native query refused')
        return out

    def top(self,scores,k=20):
        scores=np.ascontiguousarray(scores,np.float32)
        if scores.ndim!=1 or not len(scores) or not 1<=k<=20 or not np.isfinite(scores).all():
            raise ValueError('invalid top-k input')
        out=np.empty(min(k,len(scores)),np.int32)
        self.lib.fast_top(scores.ctypes.data,len(scores),len(out),out.ctypes.data)
        return out

    def query(self,text,mode=1,k=20):
        if not isinstance(text,str) or '\0' in text:
            raise ValueError('query must be text without NUL')
        raw=text.encode()
        if len(raw)>4096 or mode not in (0,1) or not isinstance(k,int) or not 1<=k<=20:
            raise ValueError('invalid query length, mode, or k')
        ids=np.empty(k,np.int32);values=np.empty(k,np.float32)
        n=self.lib.fast_query(raw,*self.refs,mode,k,ids.ctypes.data,values.ctypes.data)
        if n<0:raise ValueError('native query refused')
        return ids[:n],values[:n]


def load_fast(terms=256):
    import json
    from runtime import digest,unpack_sparse
    folder=CACHE/f'fast{terms}'
    receipt=json.loads((folder/'receipt.json').read_text())
    if digest(folder/'index.npz')!=receipt['index_sha256']:raise ValueError('fast index digest mismatch')
    with np.load(folder/'index.npz',allow_pickle=False) as data:
        engine=FastKernel(*(unpack_sparse(data,prefix,len(receipt['names'])) for prefix in ('lexical_','semantic_')))
    engine.receipt=receipt
    return engine
