import unittest
from oracle.ops.generation.run_oracles import compare_tree as r_compare_tree
from oracle.ops.generation.run_oracles import evaluate as r_evaluate
from oracle.ops.generation.run_oracles import fp as r_fp
from oracle.ops.generation.pth12.geometry import boolean_regions as g_boolean_regions
from oracle.ops.generation.pth06.geometry import polygon_area as g_polygon_area
from oracle.ops.generation.pth09.geometry import polygon_pixel_area as g_polygon_pixel_area
import random
from fractions import Fraction as F

class ProtocolAndCrossReferenceTests(unittest.TestCase):
    def test_strict_vs_accelerated_comparison(self):
        expected=r_fp(1.)
        candidate=r_fp(1.+4*2**-23)
        self.assertTrue(r_compare_tree(expected,candidate))
        self.assertFalse(r_compare_tree(expected,candidate,True))
        self.assertTrue(r_compare_tree({'id':3},{'id':4},True))
    def test_bit_input_negative_zero(self):
        request={'id':'z','op':'numeric_lookup','args':{'t':0,'table':[[{'dtype':'float64','bits':'8000000000000000'}],[1]]}}
        self.assertEqual(r_evaluate(request)['result'],[{'dtype':'float64','bits':'8000000000000000'}])
    def test_unknown_protocol_operation_rejected(self):
        with self.assertRaises(ValueError):r_evaluate({'id':'unknown','op':'path.unimplemented_strict'})
    def test_boolean_slab_independent_crosscheck(self):
        rng=random.Random(928)
        for _ in range(20):
            a=[[(F(rng.randrange(0,9),2),F(rng.randrange(0,9),2)) for _ in range(3)]]
            b=[[(F(rng.randrange(0,9),2),F(rng.randrange(0,9),2)) for _ in range(3)]]
            for op in ('union','intersection','difference','xor'):
                out=g_boolean_regions(a,b,op)
                area=sum(g_polygon_area(c) for c in out)
                self.assertGreaterEqual(area,0)
                if op in ('union','intersection','xor'):self.assertEqual(out,g_boolean_regions(b,a,op))
                if op=='union':
                    positive=[c if g_polygon_area(c)>0 else list(reversed(c)) for c in a+b]
                    fill=sum(g_polygon_pixel_area(positive,(x,y)) for y in range(4) for x in range(4))
                    self.assertEqual(area,fill)

if __name__=='__main__':unittest.main()
