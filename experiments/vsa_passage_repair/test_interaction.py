import unittest
import numpy as np
from interaction import pooled_features
class InteractionTests(unittest.TestCase):
    def test_exact_beats_orthogonal_in_exact_kernel(self):
        q=np.eye(64,dtype=np.float32)[:2];w=np.array([1,3],np.float32)
        exact=pooled_features(q,w,q);other=pooled_features(q,w,np.eye(64,dtype=np.float32)[2:4])
        self.assertGreater(exact[10],other[10]);self.assertAlmostEqual(float(exact[-3]),1,places=6)
    def test_matches_reference_and_rejects_empty(self):
        rng=np.random.default_rng(3);q=rng.normal(size=(3,64)).astype(np.float32);d=rng.normal(size=(7,64)).astype(np.float32)
        q/=np.linalg.norm(q,axis=1,keepdims=True);d/=np.linalg.norm(d,axis=1,keepdims=True);w=np.array([1,2,4],np.float32)
        s=np.clip(q@d.T,-1,1);mu=np.linspace(-1,1,11);sigma=np.full(11,.1);sigma[-1]=.001
        mass=np.exp(-.5*((s[:,:,None]-mu)/sigma)**2).sum(axis=1)
        expected=np.r_[np.average(np.log1p(mass),axis=0,weights=w),np.average(mass/len(d),axis=0,weights=w),np.average(s.max(axis=1),weights=w),s.max(axis=1).min(),np.average(s.max(axis=1)>=.5,weights=w)]
        np.testing.assert_allclose(pooled_features(q,w,d),expected,rtol=3e-5,atol=3e-5)
        with self.assertRaises(ValueError):pooled_features(q[:0],w[:0],d)
    def test_native_token_buffer_above_512(self):
        from probe import Native
        from late import Terms
        terms=Terms(Native())
        words=['zz'+chr(97+i//676)+chr(97+(i//26)%26)+chr(97+i%26) for i in range(600)]
        keys,weights=terms(' '.join(words))
        self.assertGreater(len(keys),512);self.assertEqual(len(keys),len(weights))
        with self.assertRaises(ValueError):terms('alpha\0beta')
if __name__=='__main__':unittest.main()
