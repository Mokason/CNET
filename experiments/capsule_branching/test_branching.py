import unittest
from dataclasses import replace
from branching import Cache,Context,Refusal,Spec,Split
class Fake:
    def __init__(self):self.opened=0;self.closed=0
    def open(self,spec):self.opened+=1;return spec.key
    def close(self,handle):self.closed+=1
    def ask(self,handle,spec,value):
        if value<0 or value>63:raise Refusal('coverage')
        return ((value+1)%64 if handle=='left' else (value*2)%64),1,handle
class BranchTests(unittest.TestCase):
    def setUp(self):
        self.native=Fake();self.cache=Cache(self.native,2)
        self.left=Spec('left','unused','raw_left','adjusted','a'*64)
        self.right=Spec('right','unused','raw_right','scaled','b'*64)
        self.third=Spec('third','unused','raw_extra','other','c'*64)
        self.context=Context('sample42','steady','hypothesis0')
    def tearDown(self):self.cache.close()
    def test_pins_warm_reuse_eviction(self):
        with self.cache.pin(self.left):
            with self.cache.pin(self.right):
                with self.assertRaises(Refusal):
                    with self.cache.pin(self.third):pass
            self.assertEqual(self.cache.state(self.right),'warm')
            with self.cache.pin(self.third):
                self.assertEqual(self.cache.state(self.left),'hot');self.assertEqual(self.cache.state(self.right),'cold')
        with self.cache.pin(self.left):pass
        self.assertEqual(self.native.opened,3);self.assertEqual(self.cache.peak,2)
    def test_join_and_copied_evidence(self):
        split=Split(self.cache,'r1',self.context)
        with self.cache.pin(self.left),self.cache.pin(self.right):
            a=split.branch('left',self.left,3,self.context);b=split.branch('right',self.right,5,self.context)
            result=split.join(a,b)
        with self.cache.pin(self.third):pass
        self.assertEqual(result.value,14);self.assertEqual(result.branches[0].identity,'a'*64)
        self.assertEqual(result.branches[0].units,'left')
    def test_bad_joins_refuse(self):
        split=Split(self.cache,'r1',self.context)
        with self.cache.pin(self.left),self.cache.pin(self.right):
            a=split.branch('left',self.left,3,self.context);b=split.branch('right',self.right,5,self.context)
            for bad in [replace(b,value=19),replace(b,identity='c'*64),replace(b,split_id='r2'),replace(b,output_tag='adjusted')]:
                with self.assertRaises(Refusal):split.join(a,bad)
            with self.assertRaises(Refusal):split.join(a,a)
            with self.assertRaises(Refusal):split.join(b,a)
            with self.assertRaises(Refusal):split.branch('left',self.left,3,self.context)
            with self.assertRaises(Refusal):split.branch('extra',self.left,64,self.context)
        split=Split(self.cache,'r2',self.context)
        with self.cache.pin(self.left),self.cache.pin(self.right):
            a=split.branch('left',self.left,1,self.context)
            b=split.branch('right',self.right,2,replace(self.context,hypothesis='alternative'))
            with self.assertRaises(Refusal):split.join(a,b)
if __name__=='__main__':unittest.main()
