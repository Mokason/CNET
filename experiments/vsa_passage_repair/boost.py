"""Small depth-two logistic gradient trees, trained offline with fixed settings."""
import numpy as np

def predict(model,x):
    x=np.asarray(x,dtype=np.float32)
    out=np.full(len(x),model['bias'],np.float64)
    def walk(node,idx):
        if 'value' in node:out[idx]+=node['value'];return
        left=x[idx,node['feature']]<=node['threshold']
        walk(node['left'],idx[left]);walk(node['right'],idx[~left])
    for tree in model['trees']:walk(tree,np.arange(len(x)))
    return out

def fit(x,y):
    x=np.asarray(x,dtype=np.float32);y=np.asarray(y,dtype=np.float64)
    if x.ndim!=2 or not len(x) or y.shape!=(len(x),) or not np.isfinite(x).all() or not np.isin(y,[0,1]).all():raise ValueError('invalid training data')
    prior=np.clip(y.mean(),.01,.99);model={'bias':float(np.log(prior/(1-prior))),'trees':[],'features':x.shape[1]}
    score=np.full(len(y),model['bias']);ridge=5.;rate=.15;minleaf=16
    for iteration in range(32):
        p=1/(1+np.exp(-np.clip(score,-30,30)));g=y-p;h=p*(1-p)
        def node(idx,depth):
            sg=g[idx].sum();sh=h[idx].sum()
            leaf={'value':float(rate*np.clip(sg/(sh+ridge),-2,2))}
            if depth==2 or len(idx)<2*minleaf:return leaf
            bestgain=0.;choice=None
            for j in range(x.shape[1]):
                order=idx[np.argsort(x[idx,j],kind='stable')];v=x[order,j];cg=np.cumsum(g[order]);ch=np.cumsum(h[order])
                cuts=np.arange(minleaf-1,len(idx)-minleaf)
                cuts=cuts[v[cuts]<v[cuts+1]]
                if not len(cuts):continue
                gains=cg[cuts]**2/(ch[cuts]+ridge)+(sg-cg[cuts])**2/(sh-ch[cuts]+ridge)-sg**2/(sh+ridge)
                k=int(np.argmax(gains))
                if gains[k]>bestgain:
                    cut=int(cuts[k]);bestgain=float(gains[k]);choice=(j,float((float(v[cut])+float(v[cut+1]))/2),order[:cut+1],order[cut+1:])
            if choice is None:return leaf
            j,t,left,right=choice
            return {'feature':j,'threshold':t,'left':node(left,depth+1),'right':node(right,depth+1)}
        tree=node(np.arange(len(y)),0);model['trees'].append(tree)
        score=predict(model,x)
    return model
