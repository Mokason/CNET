"""Single-thread native word interaction prototype; fixed frozen-vector projection."""
import os
os.environ['OPENBLAS_NUM_THREADS']='1'
import ctypes as ct,hashlib,subprocess
from pathlib import Path
import numpy as np
from probe import CACHE,ROOT
OUT=ROOT/'var/passage_quality_ms_20260913'
_lib=None

def pooled_features(q,w,d):
    global _lib
    q=np.ascontiguousarray(q,dtype=np.float32);w=np.ascontiguousarray(w,dtype=np.float32);d=np.ascontiguousarray(d,dtype=np.float32)
    if q.ndim!=2 or d.ndim!=2 or q.shape[1]!=64 or d.shape[1]!=64 or w.shape!=(len(q),):raise ValueError('bad word-vector shapes')
    if _lib is None:
        src=Path(__file__).with_suffix('.c');name=OUT/('interaction-'+hashlib.sha256(src.read_bytes()).hexdigest()[:16]+'.so');OUT.mkdir(exist_ok=True)
        if not name.exists():subprocess.run(['gcc','-std=c11','-O3','-march=native','-Wall','-Wextra','-Werror','-shared','-fPIC',str(src),'-lm','-o',str(name)],check=True)
        _lib=ct.CDLL(str(name));_lib.ip_pool.argtypes=[ct.c_void_p,ct.c_void_p,ct.c_int,ct.c_void_p,ct.c_int,ct.c_void_p]
    out=np.empty(25,np.float32)
    if _lib.ip_pool(q.ctypes.data,w.ctypes.data,len(q),d.ctypes.data,len(d),out.ctypes.data):raise ValueError('invalid passage evidence')
    return out

class Interaction:
    def __init__(self,native):
        from late import Terms
        self.terms=Terms(native)
        lexhash=hashlib.sha256((ROOT/'bin/registry.lex').read_bytes()).hexdigest()
        path=OUT/('projected_words-'+lexhash[:16]+'.npz')
        if not path.exists():
            native.lib.pr_vocab.argtypes=[ct.c_void_p,ct.c_void_p];n=native.lib.pr_vocab(None,None)
            keys=np.empty(n,np.uint64);vectors=np.empty((n,2048),np.int8)
            assert native.lib.pr_vocab(keys.ctypes.data,vectors.ctypes.data)==n
            rng=np.random.default_rng(20260913);projection=rng.choice(np.array([-1,1],np.float32),(2048,64))/8
            projected=vectors.astype(np.float32)@projection
            projected/=np.maximum(np.linalg.norm(projected,axis=1,keepdims=True),1e-12)
            np.savez(path,keys=keys,vectors=projected)
        with np.load(path) as a:self.ids={int(k):i for i,k in enumerate(a['keys'])};self.vectors=a['vectors']
    def encode(self,text):
        keys,w=self.terms(text);vectors=[]
        for key in keys:
            idx=self.ids.get(int(key))
            if idx is not None:vectors.append(self.vectors[idx])
            else:
                bits=np.unpackbits(np.frombuffer(hashlib.sha256(int(key).to_bytes(8,'little')).digest()[:8],np.uint8))
                vectors.append((bits.astype(np.float32)*2-1)/8)
        return np.asarray(vectors,np.float32).reshape(-1,64),w
    def prepare(self,texts):self.passages=[self.encode(t)[0] for t in texts]
    def score(self,question):
        q,w=self.encode(question)
        return np.array([pooled_features(q,w,d) for d in self.passages],np.float32)
