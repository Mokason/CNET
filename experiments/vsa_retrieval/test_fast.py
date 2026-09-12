"""Native query-path equivalence and boundary tests."""
import unittest
import numpy as np
from fast import FastKernel
from retrieval import Native, SparseIndex, reciprocal_rank_fusion

class FastTests(unittest.TestCase):
    def test_native_sparse_and_fusion(self):
        native=Native()
        docs=[native.terms(t) for t in ['apple green','apple red','orange blue']]
        a=SparseIndex(docs,[0,1,2],3)
        b=SparseIndex(docs[::-1],[0,1,2],3)
        engine=FastKernel(a,b)
        for text in ['apple','blue apple','unknownword','apple apple']:
            terms=native.terms(text)
            sa=a.score(terms,native);sb=b.score(terms,native)
            ref=np.zeros(3,np.float32)
            for branch in (sa,sb):
                for rank,c in enumerate(np.argsort(-branch,kind='stable')):
                    if branch[c]>0:ref[c]+=1/(61+rank)
            # Both branches require positive evidence in the fast sparse route.
            if not any(ref): continue
            actual=engine.scores(text)
            if text=='unknownword': np.testing.assert_array_equal(actual,0)
            else: np.testing.assert_allclose(actual,ref,atol=1e-8)
    def test_empty_and_bounds(self):
        a=SparseIndex([[1]],[0],1);engine=FastKernel(a,a)
        np.testing.assert_array_equal(engine.scores('the and'),0)
        for text in ['a\0b','x'*4097,None]:
            with self.assertRaises(ValueError):engine.scores(text)
    def test_stable_topk(self):
        a=SparseIndex([[1]],[0],1);engine=FastKernel(a,a)
        np.testing.assert_array_equal(engine.top(np.array([2,2,3,1],np.float32),3),[2,0,1])

    def test_query_includes_native_topk(self):
        native=Native()
        a=SparseIndex([native.terms('apple'),native.terms('orange')],[0,1],2)
        engine=FastKernel(a,a)
        ids,values=engine.query('orange',k=2)
        np.testing.assert_array_equal(ids,[1])
        self.assertGreater(values[0],0)
        self.assertEqual(len(engine.query('the and')[0]),0)
