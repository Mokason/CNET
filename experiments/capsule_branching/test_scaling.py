"""Behavioral checks for the isolated scaling harness."""
import unittest
from dataclasses import replace

from branching import Cache, Context, Refusal, Spec
from scaling import Node, execute, verify


class Fake:
    def __init__(self):
        self.live = self.peak = self.calls = 0

    def open(self, spec):
        self.live += 1
        self.peak = max(self.peak, self.live)
        return spec.key

    def close(self, handle):
        self.live -= 1

    def ask(self, handle, spec, value):
        self.calls += 1
        if not 0 <= value < 64:
            raise Refusal('uncovered input')
        return (value + int(spec.key) + 1) % 64, 1, spec.key


class ScalingTests(unittest.TestCase):
    def setUp(self):
        self.native = Fake()
        self.specs = [Spec(str(i), 'unused', 'state6', 'next6', str(i)) for i in range(32)]
        self.context = Context('sample', 'steady', 'actual')

    def test_large_resident_cache_and_bounded_streaming(self):
        for capacity in (4, 32):
            cache = Cache(self.native, capacity)
            nodes = [Node(i, self.specs[i], i % 64, ()) for i in range(32)]
            result = execute(cache, nodes, tuple(range(32)), 'request', self.context)
            self.assertEqual(result.value, 1024)
            self.assertLessEqual(self.native.live, capacity)
            cache.close()
            self.assertEqual(self.native.live, 0)
            verify(result)
        with self.assertRaises(ValueError):
            Cache(self.native, 33)

    def test_dependent_join_and_tamper_refusal(self):
        cache = Cache(self.native, 2)
        nodes = [Node(0, self.specs[0], 3, ()), Node(1, self.specs[1], 5, ()),
                 Node(2, self.specs[2], None, (0, 1))]
        result = execute(cache, nodes, (2,), 'request', self.context)
        self.assertEqual(result.value, 14)
        bad = replace(result, receipts=result.receipts[:-1] +
                      (replace(result.receipts[-1], input_value=12),))
        with self.assertRaises(Refusal):
            verify(bad)
        bad = replace(result, receipts=(replace(result.receipts[0], context=
                      replace(self.context, hypothesis='alternative')),)+result.receipts[1:])
        with self.assertRaises(Refusal):
            verify(bad)
        cache.close()

    def test_bad_graph_refuses_before_native_work(self):
        cache = Cache(self.native, 2)
        for nodes in ([Node(0, self.specs[0], None, (1,))],
                      [Node(0, self.specs[0], 2, ()), Node(0, self.specs[1], 3, ())],
                      [Node(0, self.specs[0], 64, ())],
                      [Node(0, replace(self.specs[0], output_tag='other'), 2, ())]):
            before = self.native.calls
            with self.assertRaises(Refusal):
                execute(cache, nodes, (0,), 'request', self.context)
            self.assertEqual(self.native.calls, before)
        cache.close()


if __name__ == '__main__':
    unittest.main()
