import unittest
import numpy as np
from boost import fit,predict
class BoostTests(unittest.TestCase):
    def test_two_feature_interaction(self):
        x=np.repeat(np.array([[0,0],[0,1],[1,0],[1,1]],np.float32),32,axis=0)
        y=np.repeat([0,0,0,1],32)
        model=fit(x,y)
        self.assertTrue(np.array_equal(predict(model,x)>0,y.astype(bool)))
    def test_empty_input_refused(self):
        with self.assertRaises(ValueError):fit(np.empty((0,2)),np.empty(0))

if __name__ == '__main__':
    unittest.main()
