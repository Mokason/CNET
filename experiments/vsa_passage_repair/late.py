"""Token alignment using an offline graph of neighbors in the frozen lexicon."""
import ctypes as ct
import numpy as np
from probe import CACHE
class Terms:
    def __init__(self,native):
        self.lib=native.lib;self.lib.pr_terms.argtypes=[ct.c_char_p,ct.c_void_p,ct.c_void_p]
    def __call__(self,text):
        # CnetVsaTokenList can hold 1024 distinct tokens; allocate its full bound.
        if not text or '\0' in text:raise ValueError('invalid query text')
        keys=np.empty(1024,np.uint64);weights=np.empty(1024,np.float32)
        n=self.lib.pr_terms(text.encode(),keys.ctypes.data,weights.ctypes.data)
        if n<0 or n>len(keys):raise ValueError('tokenization refused')
        return keys[:n],weights[:n]
class Late:
    def __init__(self,native):
        self.terms=Terms(native)
        with np.load(CACHE/'word_neighbors.npz') as a:self.keys=a['keys'];self.neighbors=a['neighbors'];self.weights=a['weights']
        self.ids={int(k):i for i,k in enumerate(self.keys)}
    def prepare(self,texts):
        self.docsets=[set(map(int,self.terms(t)[0])) for t in texts]
        self.presence=np.zeros((len(texts),len(self.keys)),bool)
        for i,keys in enumerate(self.docsets):
            ids=[self.ids[k] for k in keys if k in self.ids];self.presence[i,ids]=True
    def score(self,query):
        keys,weights=self.terms(query);out=np.zeros(len(self.docsets),np.float32);weak=np.zeros_like(out)
        for key,w in zip(keys,weights):
            idx=self.ids.get(int(key))
            if idx is None:best=np.array([int(key) in d for d in self.docsets],np.float32)
            else:best=(self.presence[:,self.neighbors[idx]]*self.weights[idx]).max(axis=1)
            out+=w*best;weak+=best>=.5
        if weights.sum()>0:out/=weights.sum()
        if len(keys):weak/=len(keys)
        return out,weak

def extend(base,aligned,coverage):
    a=aligned.astype(np.float64)
    z=(a-a.mean())/max(a.std(),1e-6)
    return np.column_stack([base,a,coverage,z,a*base[:,0]]).astype(np.float32)

def main():
    import json
    from probe import Native
    native=Native()
    native.lib.pr_vocab.argtypes=[ct.c_void_p,ct.c_void_p]
    n=native.lib.pr_vocab(None,None)
    if n <= 0 or n > 65535:raise ValueError('unsupported vocabulary size')
    keys=np.empty(n,np.uint64);vectors=np.empty((n,2048),np.int8)
    assert native.lib.pr_vocab(keys.ctypes.data,vectors.ctypes.data)==n
    import torch
    if not torch.version.hip:raise RuntimeError('neighbor generation requires ROCm PyTorch')
    v=torch.tensor(vectors,device='cuda',dtype=torch.float32)
    v=(v/torch.linalg.vector_norm(v,dim=1,keepdim=True).clamp_min(1e-12)).half()
    neighbors=[];weights=[]
    for start in range(0,n,512):
        score,index=(v[start:start+512]@v.T).topk(32,dim=1)
        neighbors.append(index.cpu().numpy().astype(np.uint16))
        weights.append(score.clamp(0,1).float().cpu().numpy())
    np.savez(CACHE/'word_neighbors.npz',keys=keys,neighbors=np.concatenate(neighbors),weights=np.concatenate(weights))
    from train import features
    rows=json.loads((CACHE/'training_labels.json').read_text());late=Late(native)
    for row in rows['rows']:
        cap=native.load(row['capsule']);late.prepare(cap.texts)
        c,o=cap.scores([row['question']]);f=extend(features(c[0],o[0],row['question'],cap.texts),*late.score(row['question']))
        row['features']=f[row['indices']].tolist();cap.close()
    (CACHE/'training_labels_late.json').write_text(json.dumps(rows,indent=2)+'\n')
if __name__=='__main__':main()
