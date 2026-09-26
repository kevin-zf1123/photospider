import unittest
import run_oracles as r
import geometry as g
import random
from fractions import Fraction as F

class ProtocolAndCrossReferenceTests(unittest.TestCase):
    def test_external_and_analytic_fixtures(self):
        report=r.fixture_check()
        self.assertTrue(report['passed'],report['failures'])
    def test_strict_vs_accelerated_comparison(self):
        expected=r.fp(1.)
        candidate=r.fp(1.+4*2**-23)
        self.assertTrue(r.compare_tree(expected,candidate))
        self.assertFalse(r.compare_tree(expected,candidate,True))
        self.assertTrue(r.compare_tree({'id':3},{'id':4},True))
    def test_bit_input_negative_zero(self):
        request={'id':'z','op':'numeric_lookup','args':{'t':0,'table':[[{'dtype':'float64','bits':'8000000000000000'}],[1]]}}
        self.assertEqual(r.evaluate(request)['result'],[{'dtype':'float64','bits':'8000000000000000'}])
    def test_unknown_protocol_operation_rejected(self):
        with self.assertRaises(ValueError):r.evaluate({'id':'unknown','op':'path.unimplemented_strict'})
    def test_boolean_slab_independent_crosscheck(self):
        rng=random.Random(928)
        for _ in range(20):
            a=[[(F(rng.randrange(0,9),2),F(rng.randrange(0,9),2)) for _ in range(3)]]
            b=[[(F(rng.randrange(0,9),2),F(rng.randrange(0,9),2)) for _ in range(3)]]
            for op in ('union','intersection','difference','xor'):
                out=g.boolean_regions(a,b,op)
                area=sum(g.polygon_area(c) for c in out)
                self.assertGreaterEqual(area,0)
                if op in ('union','intersection','xor'):self.assertEqual(out,g.boolean_regions(b,a,op))
                if op=='union':
                    positive=[c if g.polygon_area(c)>0 else list(reversed(c)) for c in a+b]
                    fill=sum(g.polygon_pixel_area(positive,(x,y)) for y in range(4) for x in range(4))
                    self.assertEqual(area,fill)

if __name__=='__main__':unittest.main()
