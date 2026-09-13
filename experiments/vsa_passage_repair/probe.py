import ctypes as ct
import hashlib,subprocess
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[2]
CACHE=ROOT/'var/passage_repair_20260913'
class Native:
    def __init__(self):
        sources=[ROOT/'src'/f'{n}.c' for n in ['cnet_vsa','cnet_vsa_bsc','cnet_vsa_text','cnet_vsa_lexicon','cnet_vsa_memory','cnet_vsa_ngram','cnet_vsa_delta','cnet_vsa_gen_capsule']]+[Path(__file__).with_suffix('.c')]
        digest=hashlib.sha256(b''.join(p.read_bytes() for p in sources+sorted((ROOT/'include').glob('*.h')))).hexdigest()[:16]
        lib=CACHE/f'probe-{digest}.so'
        if not lib.exists():subprocess.run(['gcc','-std=c11','-O3','-march=native','-Wall','-Wextra','-Werror','-fPIC','-shared','-D_GNU_SOURCE','-DCNET_HAVE_CURL=0','-include',str(ROOT/'include/cnet_platform.h'),'-I'+str(ROOT/'include'),*map(str,sources),'-lm','-pthread','-o',str(lib)],check=True)
        self.lib=ct.CDLL(str(lib));l=self.lib
        l.pr_lex.argtypes=[ct.c_char_p];l.pr_load.argtypes=[ct.c_char_p];l.pr_load.restype=ct.c_void_p
        l.pr_free.argtypes=[ct.c_void_p];l.pr_count.argtypes=[ct.c_void_p];l.pr_floor.argtypes=[ct.c_void_p];l.pr_floor.restype=ct.c_float
        l.pr_text.argtypes=[ct.c_void_p,ct.c_int];l.pr_text.restype=ct.c_char_p
        l.pr_scores.argtypes=[ct.c_void_p,ct.c_char_p,ct.c_void_p,ct.c_void_p]
        l.pr_sample.argtypes=[ct.c_char_p,ct.c_int,ct.c_void_p,ct.c_int]
        if l.pr_lex(str(ROOT/'bin/registry.lex').encode()):raise ValueError('lexicon refused')
    def load(self,name):return Capsule(self,self.lib.pr_load(str(ROOT/'bin'/f'{name}.gencap').encode()))
    def sample(self,name,rows):
        if len(rows)<512:raise ValueError('insufficient negatives')
        idx=np.empty(512,np.int32);self.lib.pr_sample(name.encode(),len(rows),idx.ctypes.data,512)
        return [rows[i] for i in idx]
class Capsule:
    def __init__(self,native,ptr):
        if not ptr:raise ValueError('capsule refused')
        self.lib=native.lib;self.ptr=ptr;self.n=self.lib.pr_count(ptr);self.floor=self.lib.pr_floor(ptr)
        self.texts=[self.lib.pr_text(ptr,i).decode() for i in range(self.n)]
    def scores(self,texts):
        cosine=np.empty((len(texts),self.n),np.float32);overlap=np.empty_like(cosine)
        for i,t in enumerate(texts):
            if not t or '\0' in t or len(t.encode())>4096:raise ValueError('invalid query')
            if self.lib.pr_scores(self.ptr,t.encode(),cosine[i].ctypes.data,overlap[i].ctypes.data)<0:raise ValueError('score refused')
        return cosine,overlap
    def close(self):
        if self.ptr:self.lib.pr_free(self.ptr);self.ptr=None
