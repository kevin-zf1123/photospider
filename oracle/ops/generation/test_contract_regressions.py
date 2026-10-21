"""Regressions for the current generation contracts, independent of the kernel."""
from fractions import Fraction as F
import math
import unittest
from unittest.mock import patch
from oracle.ops.generation.gen07.exact import bicubic_patch as e_bicubic_patch
from oracle.ops.generation.gen07.exact import bilinear as e_bilinear
from oracle.ops.generation.common.exact import float_bits as e_float_bits
from oracle.ops.generation.noi09.exact import linear_grain as e_linear_grain
from oracle.ops.generation.gen06.exact import numeric_lookup as e_numeric_lookup
from oracle.ops.generation.common.exact import power2 as e_power2
from oracle.ops.generation.pth05.exact import source_width_coordinate as e_source_width_coordinate
from oracle.ops.generation.gen06.exact import spread_rounded as e_spread_rounded
from oracle.ops.generation.gen05.exact import two_circle_rational as e_two_circle_rational
from oracle.ops.generation.pth05.exact import width_linear as e_width_linear
from oracle.ops.generation.gen08.rng import bridson_reference as rng_bridson_reference
from oracle.ops.generation.noi01.rng import pack_counter as rng_pack_counter
from oracle.ops.generation.gen08.rng import pairwise_distances as rng_pairwise_distances
from oracle.ops.generation.gen08.rng import rejection_points as rng_rejection_points
from oracle.ops.generation.run_oracles import evaluate as protocol_evaluate
from oracle.ops.generation.run_oracles import fp as protocol_fp
from oracle.ops.generation.run_oracles import main as protocol_main


class ContractRegressionTests(unittest.TestCase):
    def test_period_rounding_preserves_upper_endpoint(self):
        for dtype,p in [('float32',24),('float64',53)]:
            self.assertEqual(e_spread_rounded(-e_power2(-p-2),'repeat',dtype),1.)
            self.assertEqual(e_numeric_lookup(1,[[2],[7]]),[7])
            self.assertEqual(e_float_bits(e_spread_rounded(-3,'repeat',dtype),dtype),0)

    def test_two_circle_maximum_root_and_absence(self):
        self.assertEqual(e_two_circle_rational((2,0),(0,0),1,(4,0),1),(F(3,4),1))
        self.assertEqual(e_two_circle_rational((2,2),(0,0),1,(4,0),1),(0,0))
        with self.assertRaisesRegex(ValueError,'NoSolution'):
            e_two_circle_rational((2,2),(0,0),1,(4,0),1,'reject')

    def test_two_circle_degenerate_radius_constraint(self):
        self.assertEqual(e_two_circle_rational((0,0),(0,0),0,(1,0),1),(0,0))
        self.assertEqual(e_two_circle_rational((0,0),(1,0),1,(0,0),0),(1,1))
        with self.assertRaises(ValueError):
            e_two_circle_rational((0,0),(0,0),1,(0,0),1)
        with self.assertRaises(NotImplementedError):
            e_two_circle_rational((1,1),(0,0),0,(0,0),1)

    def test_pad_negative_zero(self):
        self.assertEqual(e_float_bits(e_spread_rounded(-0.,'pad')),1<<63)
        self.assertEqual(e_float_bits(e_spread_rounded(-0.,'repeat')),0)

    def test_color_copy_transparency_and_reference_domain(self):
        def request(a,b,t,dtype='float64'):
            return {'id':'color','op':'linear_rgba_mix','args':{'a':a,'b':b,'t':t,'dtype':dtype}}
        got=protocol_evaluate(request([-0.,0,0,1],[1,1,1,1],0))
        self.assertEqual(got['result'][0]['bits'],'8000000000000000')
        got=protocol_evaluate(request([1,0,0,0],[0,0,1,1],0))
        self.assertTrue(all(int(v['bits'],16)==0 for v in got['result']))
        # Oracle-domain rejection does not prescribe a CRV execution failure.
        with self.assertRaisesRegex(ValueError,'CRV caller precondition'):
            protocol_evaluate(request([1,0,0,2**-150],[1,0,0,2**-150],.5,'float32'))

    def test_transparent_underflow_canonical_zero(self):
        got=protocol_evaluate({'id':'tiny','op':'linear_rgba_mix','args':
            {'a':[-2**-151,0,0,2**-150],'b':[-2**-151,0,0,2**-150],
             't':.5,'dtype':'float32'}})
        self.assertEqual([v['bits'] for v in got['result']],['00000000']*4)

    def test_distinct_rational_color_probes_not_collapsed(self):
        p=1+F(1,1<<24)
        got=protocol_evaluate({'id':'rational','op':'linear_rgba_mix','args':
            {'a':[p,0,0,1],'b':[p+F(1,1<<60),0,0,1],'t':F(1,2),'dtype':'float32'}})
        self.assertEqual(got['result'][0]['bits'],'3f800001')

    def test_accelerated_cli_preserves_lookup_copies(self):
        import json,tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as tmp:
            req=Path(tmp)/'request.jsonl';actual=Path(tmp)/'candidate.jsonl'
            req.write_text(json.dumps({'id':'copy','op':'numeric_lookup','args':{'t':0,'table':[[1],[2]]}})+'\n')
            actual.write_text(json.dumps({'id':'copy','result':[protocol_fp(1+4*2**-23)]})+'\n')
            self.assertEqual(protocol_main(['--input',str(req),'--candidate',str(actual),'--profile','accelerated']),1)

    def test_mesh_four_components_have_no_alpha_semantics(self):
        controls=[[[1,0,0,0],[0,0,1,1]],[[1,0,0,0],[0,0,1,1]]]
        self.assertEqual(e_bilinear(controls,F(1,2),F(1,2)),[F(1,2),0,F(1,2),F(1,2)])
        controls=[[[i,j,i-j,i+j] for i in range(4)] for j in range(4)]
        self.assertEqual(e_bicubic_patch(controls,F(1,2),F(1,2)),[F(3,2),F(3,2),0,3])

    def test_source_width_domains_and_trim(self):
        lengths=[100,300]
        self.assertEqual(e_source_width_coordinate(lengths,1,150,'pathset_normalized_arclength'),F(5,8))
        self.assertEqual(e_source_width_coordinate(lengths,1,150,'subpath_normalized_arclength'),F(1,2))
        self.assertEqual(e_source_width_coordinate(lengths,1,150,'subpath_arclength_px'),150)
        # Retaining [25,75] of a 100px source samples widths 4 and 8, not 2 and 10.
        actual=[e_width_linear(e_source_width_coordinate([100],0,s,'subpath_normalized_arclength'),
                               1,[0,1],[2,10]) for s in (25,75)]
        self.assertEqual(actual,[4,8])

    def test_candidate_counter_full_uint32_fields(self):
        m=(1<<32)-1
        c=rng_pack_counter(-(1<<31),(1<<31)-1,65535,255,m,m,m)
        self.assertEqual(c,(0x7fffffff80000000,(1<<64)-1,(1<<56)-1,0))
        for name in ('stream','frame','draw'):
            with self.assertRaises(ValueError):rng_pack_counter(0,0,**{name:1<<32})
        # Hand decoding demonstrates field separation, not output independence.
        c=rng_pack_counter(-7,13,39,10,100000,200000,300000)
        self.assertEqual(c[0] & m,(-7)&m);self.assertEqual(c[0]>>32,13)
        self.assertEqual(c[1] & m,200000);self.assertEqual(c[1]>>32,300000)
        self.assertEqual(c[2] & m,100000);self.assertEqual((c[2]>>32)&65535,39)
        self.assertEqual(c[2]>>48,10)

    def test_uniform_protocol_finite_grid(self):
        q={'id':'uniform','op':'uniform','args':{'x':0,'y':0}}
        got=protocol_evaluate(q)
        self.assertEqual(got['quality'],'Exact_finite_reference')
        self.assertEqual(got['result']['dtype'],'float64')
        # Nonnegative finite half-open sample, with IEEE ordering for positive bits.
        self.assertLess(int(got['result']['bits'],16),0x3ff0000000000000)
        with self.assertRaises(TypeError):
            protocol_evaluate({'id':'invalid-draw','op':'uniform','args':{'x':0,'y':0,'draw':1}})
        core=protocol_evaluate({'id':'core','op':'philox','args':{'counter':[0]*4,'key':[0]*2}})
        self.assertEqual(core['quality'],'Exact_finite_reference')

    def test_rejection_normal_stops_are_not_resource_failures(self):
        a=rng_rejection_points([0,0,1,1],3,20,max_count=100,with_status=True)
        self.assertEqual(a['stop_reason'],'candidates_exhausted');self.assertEqual(len(a['points']),1)
        a=rng_rejection_points([0,0,5,5],F(1,10),100,max_count=3,with_status=True)
        self.assertEqual(a['stop_reason'],'count_reached');self.assertEqual(len(a['points']),3)
        self.assertTrue(rng_pairwise_distances(a['points'],F(1,10)))

    def test_bridson_normal_stops_and_hard_budget(self):
        a=rng_bridson_reference([0,0,1,1],3,k=4,max_count=100,with_status=True)
        self.assertEqual(a['stop_reason'],'active_exhausted');self.assertEqual(len(a['points']),1)
        a=rng_bridson_reference([0,0,1,1],F(1,10),max_count=1,max_iterations=0,with_status=True)
        self.assertEqual(a['stop_reason'],'count_reached')
        with self.assertRaises(ArithmeticError):
            rng_bridson_reference([0,0,1,1],F(1,10),max_count=2,max_iterations=0)

    def test_zero_target_stops_before_random_sampling(self):
        with patch('oracle.ops.generation.gen08.rng.block',side_effect=AssertionError('unexpected random sample')):
            for result in (
                rng_rejection_points([0,0,1,1],1,10,max_count=0,with_status=True),
                rng_bridson_reference([0,0,1,1],1,max_count=0,max_iterations=0,with_status=True),
            ):
                self.assertEqual(result,{'points':[],'stop_reason':'count_reached'})

    def test_grain_hidden_color_and_zero_strength_copy(self):
        rgb,alpha=e_linear_grain([F(1,5),F(3,10),F(2,5)],-0.,1,F(1,10))
        self.assertEqual(rgb,[F(3,10),F(2,5),F(1,2)])
        self.assertEqual(math.copysign(1,alpha),-1)
        rgb,alpha=e_linear_grain([-0.,2.,3.],-0.,0,7)
        self.assertEqual(e_float_bits(rgb[0]),1<<63)
        self.assertEqual(e_float_bits(alpha),1<<63)


if __name__=='__main__':unittest.main()
