"""Regressions for the current generation contracts, independent of the kernel."""
from fractions import Fraction as F
import math
import unittest
import exact as e
import rng
import run_oracles as protocol


class ContractRegressionTests(unittest.TestCase):
    def test_period_rounding_preserves_upper_endpoint(self):
        for dtype,p in [('float32',24),('float64',53)]:
            self.assertEqual(e.spread_rounded(-e.power2(-p-2),'repeat',dtype),1.)
            self.assertEqual(e.numeric_lookup(1,[[2],[7]]),[7])
            self.assertEqual(e.float_bits(e.spread_rounded(-3,'repeat',dtype),dtype),0)

    def test_two_circle_maximum_root_and_absence(self):
        self.assertEqual(e.two_circle_rational((2,0),(0,0),1,(4,0),1),(F(3,4),1))
        self.assertEqual(e.two_circle_rational((2,2),(0,0),1,(4,0),1),(0,0))
        with self.assertRaisesRegex(ValueError,'NoSolution'):
            e.two_circle_rational((2,2),(0,0),1,(4,0),1,'reject')

    def test_two_circle_degenerate_radius_constraint(self):
        self.assertEqual(e.two_circle_rational((0,0),(0,0),0,(1,0),1),(0,0))
        self.assertEqual(e.two_circle_rational((0,0),(1,0),1,(0,0),0),(1,1))
        with self.assertRaises(ValueError):
            e.two_circle_rational((0,0),(0,0),1,(0,0),1)
        with self.assertRaises(NotImplementedError):
            e.two_circle_rational((1,1),(0,0),0,(0,0),1)

    def test_pad_negative_zero(self):
        self.assertEqual(e.float_bits(e.spread_rounded(-0.,'pad')),1<<63)
        self.assertEqual(e.float_bits(e.spread_rounded(-0.,'repeat')),0)

    def test_color_copy_transparency_and_underflow(self):
        def request(a,b,t,dtype='float64'):
            return {'id':'color','op':'linear_rgba_mix','args':{'a':a,'b':b,'t':t,'dtype':dtype}}
        got=protocol.evaluate(request([-0.,0,0,1],[1,1,1,1],0))
        self.assertEqual(got['result'][0]['bits'],'8000000000000000')
        got=protocol.evaluate(request([1,0,0,0],[0,0,1,1],0))
        self.assertTrue(all(int(v['bits'],16)==0 for v in got['result']))
        with self.assertRaisesRegex(ArithmeticError,'AssociationUnderflow'):
            protocol.evaluate(request([1,0,0,2**-150],[1,0,0,2**-150],.5,'float32'))

    def test_transparent_underflow_canonical_zero(self):
        got=protocol.evaluate({'id':'tiny','op':'linear_rgba_mix','args':
            {'a':[-2**-151,0,0,2**-150],'b':[-2**-151,0,0,2**-150],
             't':.5,'dtype':'float32'}})
        self.assertEqual([v['bits'] for v in got['result']],['00000000']*4)

    def test_distinct_rational_color_probes_not_collapsed(self):
        p=1+F(1,1<<24)
        got=protocol.evaluate({'id':'rational','op':'linear_rgba_mix','args':
            {'a':[p,0,0,1],'b':[p+F(1,1<<60),0,0,1],'t':F(1,2),'dtype':'float32'}})
        self.assertEqual(got['result'][0]['bits'],'3f800001')

    def test_accelerated_cli_preserves_lookup_copies(self):
        import json,tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as tmp:
            req=Path(tmp)/'request.jsonl';actual=Path(tmp)/'candidate.jsonl'
            req.write_text(json.dumps({'id':'copy','op':'numeric_lookup','args':{'t':0,'table':[[1],[2]]}})+'\n')
            actual.write_text(json.dumps({'id':'copy','result':[protocol.fp(1+4*2**-23)]})+'\n')
            self.assertEqual(protocol.main(['--input',str(req),'--candidate',str(actual),'--profile','accelerated']),1)

    def test_mesh_four_components_have_no_alpha_semantics(self):
        controls=[[[1,0,0,0],[0,0,1,1]],[[1,0,0,0],[0,0,1,1]]]
        self.assertEqual(e.bilinear(controls,F(1,2),F(1,2)),[F(1,2),0,F(1,2),F(1,2)])
        controls=[[[i,j,i-j,i+j] for i in range(4)] for j in range(4)]
        self.assertEqual(e.bicubic_patch(controls,F(1,2),F(1,2)),[F(3,2),F(3,2),0,3])

    def test_source_width_domains_and_trim(self):
        lengths=[100,300]
        self.assertEqual(e.source_width_coordinate(lengths,1,150,'pathset_normalized_arclength'),F(5,8))
        self.assertEqual(e.source_width_coordinate(lengths,1,150,'subpath_normalized_arclength'),F(1,2))
        self.assertEqual(e.source_width_coordinate(lengths,1,150,'subpath_arclength_px'),150)
        # Retaining [25,75] of a 100px source samples widths 4 and 8, not 2 and 10.
        actual=[e.width_linear(e.source_width_coordinate([100],0,s,'subpath_normalized_arclength'),
                               1,[0,1],[2,10]) for s in (25,75)]
        self.assertEqual(actual,[4,8])

    def test_candidate_counter_full_uint32_fields(self):
        m=(1<<32)-1
        c=rng.pack_counter(-(1<<31),(1<<31)-1,65535,255,m,m,m)
        self.assertEqual(c,(0x7fffffff80000000,(1<<64)-1,(1<<56)-1,0))
        for name in ('stream','frame','draw'):
            with self.assertRaises(ValueError):rng.pack_counter(0,0,**{name:1<<32})
        # Hand decoding demonstrates field separation, not output independence.
        c=rng.pack_counter(-7,13,39,10,100000,200000,300000)
        self.assertEqual(c[0] & m,(-7)&m);self.assertEqual(c[0]>>32,13)
        self.assertEqual(c[1] & m,200000);self.assertEqual(c[1]>>32,300000)
        self.assertEqual(c[2] & m,100000);self.assertEqual((c[2]>>32)&65535,39)
        self.assertEqual(c[2]>>48,10)

    def test_candidate_protocol_requires_opt_in(self):
        q={'id':'candidate','op':'uniform','args':{'x':0,'y':0}}
        with self.assertRaisesRegex(ValueError,'not frozen'):protocol.evaluate(q)
        got=protocol.evaluate(q,allow_candidate_rng=True)
        self.assertIn('Candidate_rng_layout_not_production_golden',got['quality'])
        # The algorithm core KAT is independent of experimental address packing.
        core=protocol.evaluate({'id':'core','op':'philox','args':{'counter':[0]*4,'key':[0]*2}})
        self.assertEqual(core['quality'],'Exact_finite_reference')

    def test_rejection_normal_stops_are_not_resource_failures(self):
        a=rng.rejection_points([0,0,1,1],3,20,max_count=100,with_status=True)
        self.assertEqual(a['stop_reason'],'candidates_exhausted');self.assertEqual(len(a['points']),1)
        a=rng.rejection_points([0,0,5,5],F(1,10),100,max_count=3,with_status=True)
        self.assertEqual(a['stop_reason'],'count_reached');self.assertEqual(len(a['points']),3)
        self.assertTrue(rng.pairwise_distances(a['points'],F(1,10)))

    def test_bridson_normal_stops_and_hard_budget(self):
        a=rng.bridson_reference([0,0,1,1],3,k=4,max_count=100,with_status=True)
        self.assertEqual(a['stop_reason'],'active_exhausted');self.assertEqual(len(a['points']),1)
        a=rng.bridson_reference([0,0,1,1],F(1,10),max_count=1,max_iterations=0,with_status=True)
        self.assertEqual(a['stop_reason'],'count_reached')
        with self.assertRaises(ArithmeticError):
            rng.bridson_reference([0,0,1,1],F(1,10),max_count=2,max_iterations=0)

    def test_grain_hidden_color_and_zero_strength_copy(self):
        rgb,alpha=e.linear_grain([F(1,5),F(3,10),F(2,5)],-0.,1,F(1,10))
        self.assertEqual(rgb,[F(3,10),F(2,5),F(1,2)])
        self.assertEqual(math.copysign(1,alpha),-1)
        rgb,alpha=e.linear_grain([-0.,2.,3.],-0.,0,7)
        self.assertEqual(e.float_bits(rgb[0]),1<<63)
        self.assertEqual(e.float_bits(alpha),1<<63)


if __name__=='__main__':unittest.main()
