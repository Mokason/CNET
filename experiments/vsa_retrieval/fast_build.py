"""Compile model whole-word aliases to CNET stems using only the model vocabulary."""
import os
os.environ['OPENBLAS_NUM_THREADS']='1'
import argparse,json
from collections import defaultdict
import numpy as np
from tokenizers import Tokenizer
from retrieval import CACHE,Native,SparseIndex
from runtime import unpack_sparse,digest


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--terms',type=int,choices=[256,1024],default=256);args=ap.parse_args()
    folder=CACHE/f'runtime{args.terms}'
    receipt=json.loads((folder/'receipt.json').read_text())
    for name in ('index.npz','tokenizer.json'):
        if digest(folder/name)!=receipt['sha256'][name]:raise ValueError('source cache changed')
    native=Native();vocab=Tokenizer.from_file(str(folder/'tokenizer.json')).get_vocab()
    aliases={}
    for word,token in vocab.items():
        if word.startswith(('##','[')) or not word.isalpha():continue
        keys=native.terms(word)
        if len(keys)==1:aliases[token]=keys[0]
    with np.load(folder/'index.npz',allow_pickle=False) as data:
        nc=len(receipt['names']);lexical=unpack_sparse(data,'lexical_',nc);semantic=unpack_sparse(data,'semantic_',nc)
        pooled=defaultdict(dict)
        for token,key in sorted(aliases.items()):
            start,end=semantic.offset[token:token+2]
            target=pooled[key]
            for c,w in zip(semantic.owners[start:end],semantic.weights[start:end]):target[int(c)]=max(target.get(int(c),0),float(w))
        keys=np.array(sorted(pooled),np.uint64);owners=[];weights=[];offset=[0]
        for key in keys:
            for c,w in sorted(pooled[int(key)].items()):owners.append(c);weights.append(w)
            offset.append(len(owners))
        arrays={'semantic_keys':keys,'semantic_offset':np.array(offset,np.uint32),'semantic_owners':np.array(owners,np.int32),'semantic_weights':np.array(weights,np.float32)}
        arrays.update({'lexical_'+k:getattr(lexical,k) for k in ('keys','offset','owners','weights')})
    target=CACHE/f'fast{args.terms}';target.mkdir(exist_ok=True)
    np.savez(target/'index.npz',**arrays)
    output={'names':receipt['names'],'source_receipt_sha256':digest(folder/'receipt.json'),'index_sha256':digest(target/'index.npz'),'runtime_data_bytes':sum(a.nbytes for a in arrays.values())+len(json.dumps(receipt['names']).encode()),'model_revision':receipt['model_revision'],'whole_word_aliases':len(aliases),'canonical_keys':len(keys),'semantic_postings':len(owners),'scope':'Uncertified stem-keyed sparse retrieval; no lexicon or neural runtime required.'}
    (target/'receipt.json').write_text(json.dumps(output,indent=2)+'\n')
    print(args.terms,output['runtime_data_bytes']/2**20,'MiB',len(owners),'postings')

if __name__=='__main__':main()
