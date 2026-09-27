#!/usr/bin/env python3
"""Executable evidence beyond self-generated golden files.

Hand-derived exact vectors; IEEE midpoint tests; independent MPFR directed
certificates; optional SciPy/scikit-image differential checks with aligned
mathematical conventions. External float results are never strict goldens.
"""
from __future__ import annotations
from fractions import Fraction as Q
from pathlib import Path
import argparse
import importlib.metadata
import itertools
import json
import math
import platform
import random
import sys
import time
import unittest
import reference as r
from exact import (rn,bits,from_bits,pow2,sqrt_exact,Quadratic,OracleError,
                   accelerated_ok,atom)
from run_cases import outcome,verify_hand
from fixtures import examples
from gaussian import MPFR, fraction_bounds

HERE=Path(__file__).resolve().parent
COUNTS={}
try:
    import numpy as np
    from scipy import ndimage as ndi
    import skimage.morphology as skm
    EXTERNAL=True
except ImportError:
    EXTERNAL=False

class AnalyticTests(unittest.TestCase):
    def test_all_members_hand_derived(self):
        cs=examples()
        self.assertEqual({c['op'] for c in cs},set(r.REGISTRY))
        for c in cs:
            with self.subTest(case=c['id']): verify_hand(c,outcome(c))
        COUNTS['hand_derived_and_error_vectors']=len(cs)

    def test_checked_in_vectors(self):
        cs=json.loads((HERE/'cases.json').read_text())['cases']
        self.assertEqual(cs,examples())
        actual={c['id']:outcome(c) for c in cs}
        self.assertEqual(actual,json.loads((HERE/'golden.json').read_text())['results'])

    def test_rational_ieee_midpoints(self):
        for dtype,p,emin,emax,width in [('float32',24,-126,127,32),('float64',53,-1022,1023,64)]:
            self.assertEqual(bits(rn(Q(1)+pow2(-p),dtype),dtype),bits(1.,dtype))
            self.assertEqual(bits(rn(Q(1)+3*pow2(-p),dtype),dtype),bits(1.,dtype)+2)
            self.assertEqual(bits(rn(pow2(emin-p),dtype),dtype),0)
            self.assertEqual(bits(rn(-pow2(emin-p),dtype),dtype),1<<(width-1))
            self.assertEqual(bits(rn(3*pow2(emin-p),dtype),dtype),2)
            maxval=Q(from_bits(((1<<(width-p))-2)<<(p-1) | ((1<<(p-1))-1),dtype))
            self.assertTrue(math.isfinite(rn(maxval,dtype)))
            with self.assertRaises(OracleError): rn(pow2(emax+1),dtype)
        # Binary64 intermediate loses a perturbation of a Float32 midpoint.
        value=Q(1)+pow2(-24)+pow2(-80)
        self.assertEqual(bits(rn(value,'float32'),'float32'),bits(1.,'float32')+1)
        self.assertEqual(bits(float(value),'float32'),bits(1.,'float32'))
        self.assertEqual(bits(r.offset_discrete([[-0.]],radius=0)[0][0]),1<<63)
        self.assertEqual(bits(r.offset_discrete([[-0.]],radius=-0.)[0][0]),1<<63)
        COUNTS['ieee_boundary_assertions']=18

    def test_random_rationals_vs_mpfr(self):
        rng=random.Random(74302)
        for i in range(300):
            q=Q(rng.randrange(-(1<<80),1<<80),rng.randrange(1,1<<60))*pow2(rng.randrange(-80,80))
            with MPFR(256) as m:
                lo,hi=fraction_bounds(m,q)
                for dtype,width in [('float32',4),('float64',8)]:
                    lb,ub=m.bits(lo,width),m.bits(hi,width)
                    self.assertEqual(lb,ub,'directed bounds must certify this fixture')
                    self.assertEqual(bits(rn(q,dtype),dtype),lb)
        COUNTS['mpfr_rational_certificates']=600

    def test_quadratic_vs_directed_mpfr(self):
        rng=random.Random(91)
        for i in range(150):
            a,b,q=Q(rng.randrange(-100,100),7),Q(rng.randrange(1,30),5),Q(rng.randrange(1,500),13)
            exact=Quadratic(a,b,q)
            with MPFR(256) as m:
                al,au=fraction_bounds(m,a);bl,bu=fraction_bounds(m,b);ql,qu=fraction_bounds(m,q)
                lo=m.binary('add',al,m.binary('mul',bl,m.unary('sqrt',ql,m.downward),m.downward),m.downward)
                hi=m.binary('add',au,m.binary('mul',bu,m.unary('sqrt',qu,m.upward),m.upward),m.upward)
                for dtype,width in [('float32',4),('float64',8)]:
                    lb,ub=m.bits(lo,width),m.bits(hi,width)
                    self.assertEqual(lb,ub)
                    self.assertEqual(bits(rn(exact,dtype),dtype),lb)
        COUNTS['mpfr_quadratic_certificates']=300

    def test_accelerated_inherited_bounds(self):
        for dtype in ('float32','float64'):
            self.assertTrue(accelerated_ok(1.,1.,dtype))
            self.assertFalse(accelerated_ok(-0.,0.,dtype))
            self.assertFalse(accelerated_ok(math.nan,0.,dtype))
        one=bits(1.,'float32')
        self.assertTrue(accelerated_ok(from_bits(one+4,'float32'),1.,'float32'))
        self.assertFalse(accelerated_ok(from_bits(one+5,'float32'),1.,'float32'))
        bound=4*2**-23
        self.assertTrue(accelerated_ok(1+bound,1.,'float64'))
        self.assertFalse(accelerated_ok(math.nextafter(1+bound,math.inf),1.,'float64'))
        self.assertFalse(accelerated_ok(1+bound,1.,'float64',exact=True))
        self.assertFalse(accelerated_ok(2**-130+2**-150,2**-130,'float64'))

    def test_small_binary_morphology_laws(self):
        for word in range(256):
            a=[[int(word>>(y*4+x)&1) for x in range(4)] for y in range(2)]
            for footprint in ('square','diamond','disk'):
                d=r.dilate(a,radius=1,footprint=footprint,dtype='float64')
                e=r.erode(a,radius=1,footprint=footprint,dtype='float64')
                o=r.opening(a,radius=1,footprint=footprint,dtype='float64')
                c=r.closing(a,radius=1,footprint=footprint,dtype='float64')
                for y,x in r.coords(a):
                    self.assertLessEqual(e[y][x],a[y][x]);self.assertGreaterEqual(d[y][x],a[y][x])
                    self.assertLessEqual(o[y][x],a[y][x]);self.assertGreaterEqual(c[y][x],a[y][x])
                self.assertEqual(r.opening(o,radius=1,footprint=footprint,dtype='float64'),o)
                self.assertEqual(r.closing(c,radius=1,footprint=footprint,dtype='float64'),c)
        COUNTS['binary_morphology_configurations']=768

    def test_topology_invariants_exhaustive_3x3(self):
        for word in range(512):
            a=[[int(word>>(y*3+x)&1) for x in range(3)] for y in range(3)]
            out=r.thin_topological(a,dtype='float64')
            self.assertEqual(r.topology(a),r.topology(out))
            self.assertEqual(r.thin_topological(out,dtype='float64'),out)
            for y,x in r.coords(a): self.assertLessEqual(out[y][x],a[y][x])
        COUNTS['topology_exhaustive_inputs']=512

    def test_edt_exact_squared_anisotropy_tie_limit(self):
        a=[[1,0,0],[0,0,1]]
        got=r.nearest_feature(a,sy=2,sx=3,squared=True,exterior='none',dtype='float64')
        # At(0,1), distances squared to(0,0)/(1,2) are9 and13.
        self.assertEqual(got['distance'][0][1],9)
        self.assertEqual([p[0][1] for p in got['nearest']],[0,0])
        a=[[1,0,1]]
        self.assertEqual(r.nearest_feature(a)['nearest'][1][0][1],0)
        self.assertEqual(r.truncated_nearest_feature(a,limit=1)['within'],[[1,1,1]])
        self.assertEqual(r.truncated_nearest_feature(a,limit=math.nextafter(1,0))['within'],[[1,0,1]])
        for metric in ('l1','l2','linf'):
            out=r.signed_center_distance([[1]],metric=metric,sy=2,sx=3)
            self.assertEqual(out,dict(distance=[[-2.]]))

    def test_gaussian_nontrivial_certificates(self):
        from gaussian import certified_gaussian
        # Independent algebraic reduction of1x1 zero extension:1/(1+2 exp(-1/2))^2.
        for dtype,width in [('float32',4),('float64',8)]:
            with MPFR(256) as m:
                x=m.binary('div',m.integer(-1),m.integer(2),m.nearest)
                el=m.unary('exp',x,m.downward);eu=m.unary('exp',x,m.upward)
                dl=m.binary('add',m.integer(1),m.binary('mul',m.integer(2),el,m.downward),m.downward)
                du=m.binary('add',m.integer(1),m.binary('mul',m.integer(2),eu,m.upward),m.upward)
                lo=m.binary('div',m.integer(1),m.binary('mul',du,du,m.upward),m.downward)
                hi=m.binary('div',m.integer(1),m.binary('mul',dl,dl,m.downward),m.upward)
                self.assertEqual(m.bits(lo,width),m.bits(hi,width))
                self.assertEqual(bits(r.gaussian_feather([[1]],radius_y=1,radius_x=1,dtype=dtype)[0][0],dtype),m.bits(lo,width))
            inp=[[0,.25,1],[1,.5,0]]
            out=r.gaussian_feather(inp,sigma_y=.5,sigma_x=2,radius_y=1,radius_x=2,boundary='reflect_half',dtype=dtype)
            self.assertTrue(all(0<=x<=1 for row in out for x in row))
        # Arbitrarily tiny positive sigma must retain mathematical taps but round to identity here.
        tiny=from_bits(1,'float64')
        self.assertEqual(r.gaussian_feather([[1]],sigma_y=tiny,sigma_x=tiny,radius_y=1,radius_x=1),[[1.]])

    def test_mask_storage_and_distance_infinity_contract(self):
        for dtype in ('float32', 'float64'):
            with self.subTest(dtype=dtype):
                empty=r.nearest_feature([[0]],dtype=dtype,output_dtype=dtype,exterior='none')
                self.assertEqual(empty,dict(distance=[[math.inf]],nearest=[[[-1]],[[-1]]]))
                self.assertEqual(r.signed_center_distance([[1]],dtype=dtype,output_dtype=dtype,
                                 exterior='none'),dict(distance=[[-math.inf]]))
                self.assertEqual(r.signed_center_distance([[0]],dtype=dtype,output_dtype=dtype,
                                 exterior='none'),dict(distance=[[math.inf]]))
                self.assertEqual(r.truncated_signed_distance([[1]],limit=2,dtype=dtype,
                                 output_dtype=dtype,exterior='none'),dict(distance=[[-2.]],within=[[0]]))
                self.assertEqual(r.shift_distance_field([[-math.inf,math.inf]],radius=2,dtype=dtype),
                                 [[-math.inf,math.inf]])
                self.assertEqual(r.threshold_distance_field([[-math.inf,math.inf]],dtype=dtype),[[1.,0.]])
                for fn in (r.distance_feather_linear,r.distance_feather_smoothstep):
                    for widths in ((0,0),(0,1),(1,0),(1,1)):
                        self.assertEqual(fn([[-math.inf,math.inf]],inner=widths[0],outer=widths[1],dtype=dtype),[[1.,0.]])
                    with self.assertRaises(OracleError): fn([[math.nan]],dtype=dtype)
                self.assertEqual(r.nonzero_to_binary([[0,128,255]],output_dtype=dtype),[[0.,1.,1.]])
                # A finite but unrepresentable squared distance is an error, not no-feature Inf.
                spacing=1e30 if dtype=='float32' else 1e200
                with self.assertRaises(OracleError):
                    r.nearest_feature([[1,0]],sx=spacing,squared=True,dtype=dtype,output_dtype=dtype)
        for fn in (r.invert,r.dilate,r.label_compact,r.signed_center_distance):
            with self.assertRaises(OracleError) as caught: fn([[1]],dtype='uint8')
            self.assertEqual(caught.exception.code,'TypeMismatch')
        with self.assertRaises(OracleError): r.threshold([[math.inf]])

    def test_polygon_sampling_profiles(self):
        square=[[-.5,-.5],[-.5,.5],[.5,.5],[.5,-.5]]
        for kw in (dict(sample_pattern='bad'),
                   dict(sample_pattern='grid_center',samples_per_axis=2,sample_count=4),
                   dict(sample_pattern='grid_center'),
                   dict(sample_pattern='vulkan_standard',sample_count=3),
                   dict(sample_pattern='vulkan_standard',sample_count=4,samples_per_axis=2)):
            with self.assertRaises(OracleError): r.offset_polygon_grid(square,1,1,**kw)
        # Asymmetric rectangle isolates the (1/8,5/8) standard 4x location.
        rectangle=[[0.,-.4375],[0.,-.3125],[.25,-.3125],[.25,-.4375]]
        self.assertEqual(r.offset_polygon_grid(rectangle,1,1,sample_pattern='vulkan_standard',
                                              sample_count=4),[[.25]])
        self.assertEqual(r.offset_polygon_grid(rectangle,1,1,sample_pattern='grid_center',
                                              samples_per_axis=2),[[0.]])

    def test_standard_thinning_fixed_points(self):
        for fn in (r.thin_zhang_suen,r.thin_guo_hall):
            for word in range(512):
                a=[[float((word>>(3*y+x))&1) for x in range(3)] for y in range(3)]
                out=fn(a)
                self.assertEqual(fn(out),out)
                self.assertTrue(all(out[y][x]<=a[y][x] for y in range(3) for x in range(3)))
                self.assertEqual(fn(a,dtype='float32'),out)
            with self.assertRaises(OracleError):fn([[.5]])
            with self.assertRaises(OracleError):fn([[1]],dtype='uint8')
        COUNTS['standard_thinning_fixed_point_inputs']=1024

    def test_maximal_ball_reconstruction(self):
        for metric in ('l1','l2','linf'):
            for sy,sx in ((1,1),(2,3)):
                for word in range(64):
                    a=[[float((word>>(3*y+x))&1) for x in range(3)] for y in range(2)]
                    out=r.medial_axis_maximal_balls(a,metric=metric,sy=sy,sx=sx)
                    foreground={(y,x) for y in range(2) for x in range(3) if a[y][x]}
                    # Build balls in an independently enumerated rectangle including background.
                    universe={(y,x) for y in range(-1,3) for x in range(-1,4)}
                    def score(p,q):
                        dy,dx=abs(p[0]-q[0])*sy,abs(p[1]-q[1])*sx
                        return dy+dx if metric=='l1' else max(dy,dx) if metric=='linf' else dy*dy+dx*dx
                    balls={p:{q for q in universe if score(p,q)<min(score(p,b) for b in universe-foreground)} for p in foreground}
                    selected={p for p in foreground if out['axis'][p[0]][p[1]]}
                    self.assertEqual(set().union(*(balls[p] for p in selected)),foreground)
                    for p in foreground:
                        dominated=any(balls[p].issubset(b) and balls[p]!=b for b in balls.values())
                        self.assertEqual(p in selected,not dominated)
        tiny=float.fromhex('0x0.0000000000001p-1022')
        self.assertEqual(r.medial_axis_maximal_balls([[1]],sy=tiny,sx=tiny,output_dtype='float32'),
                         dict(axis=[[1.]],radius=[[0.]]))
        self.assertEqual(r.medial_axis_maximal_balls([[1]],sy=1e300,sx=1e300,output_dtype='float32',request='axis'),dict(axis=[[1.]]))
        with self.assertRaises(OracleError):r.medial_axis_maximal_balls([[1]],sy=1e300,sx=1e300,output_dtype='float32')
        full=r.medial_axis_maximal_balls([[1]*3 for _ in range(3)],metric='l1')
        self.assertEqual(full['axis'],[[1.,0.,1.],[0.,1.,0.],[1.,0.,1.]])
        COUNTS['maximal_ball_reconstruction_inputs']=384

    def test_group_statistics_exact_model(self):
        from decimal import Decimal, localcontext
        from color_statistics import fit,fit_image,apply
        table=[[0,0],[2,2]]
        got=fit(table,[5,5],[1,1],[1,1])
        with localcontext() as ctx:
            ctx.prec=100
            expected=[[float(Decimal(2).sqrt()),0.],
                      [float(Decimal(1)/Decimal(2).sqrt()),float((Decimal(3)/2).sqrt())]]
        self.assertEqual(got,dict(model=dict(ids=[5],mean=[[1.,1.]],cholesky=[expected]),skipped_group_ids=[]))
        self.assertEqual(fit(table[::-1],[5,5],[4,4],[1,1]),got)
        self.assertEqual(fit_image([[[0,2]],[[0,2]]],[[5,5]],[[1,1]],[1,1],'0,1'),got)
        skipped=fit([[math.nan,math.inf],[2,3]],[0,8],[math.nan,0],[1,1])
        self.assertEqual(skipped,dict(model=dict(ids=[],mean=[],cholesky=[]),skipped_group_ids=[8]))
        with self.assertRaises(OracleError):fit([[math.nan]],[1],[0],[1])
        with self.assertRaises(OracleError):fit([[1]],[1],[-1],[1])
        with self.assertRaises(OracleError):fit([[1]],[-1],[1],[1])
        with self.assertRaises(OracleError):fit([[1]],[1],[1],[0])
        self.assertEqual(apply([[[math.nan]]],skipped['model'],'0',0,1),[[0.]])
        one=fit([[0]],[1],[1],[1])['model']
        duplicate=dict(ids=[1,2],mean=one['mean']*2,cholesky=one['cholesky']*2)
        self.assertEqual(apply([[[.5]]],one,'0',0,1),apply([[[.5]]],duplicate,'0',0,1))
        bad=dict(ids=[1],mean=[[0]],cholesky=[[[0]]])
        with self.assertRaises(OracleError):apply([[[0]]],bad,'0',0,1)
        bad=dict(ids=[1],mean=[[0,0]],cholesky=[[[1,-0.],[0,1]]])
        with self.assertRaises(OracleError):apply([[[0]],[[0]]],bad,'0,1',0,1)
        with self.assertRaises(OracleError):apply([[[0]]],{},'0',0,1)

    def test_ciede2000_certificates(self):
        from ciede2000 import certify
        # Author supplementary decimal values independently check formula branches.
        pairs=[([50,2.6772,-79.7751],[50,0,-82.7485],2.0425),
               ([50,3.1571,-77.2803],[50,0,-82.7485],2.8615),
               ([50,0,0],[50,-1,2],2.3669),
               ([50,2.49,-.001],[50,-2.49,.001],7.1792)]
        for a,b,want in pairs:
            p,q=[Q(v) for v in a],[Q(v) for v in b]
            for dtype in ('float32','float64'):
                got=certify(p,q,Q(1),Q(1),Q(1),dtype)
                self.assertLess(abs(got-want),.000051)
                self.assertEqual(got,certify(q,p,Q(1),Q(1),Q(1),dtype))
        for policy,want in [('include',1.),('exclude',0.)]:
            self.assertEqual(r.hue_range([[[.5]],[[0]]],minimum_chroma=0,neutral_policy=policy,inner=0,outer=.1),[[want]])
        self.assertEqual(r.hue_range([[[.5]],[[.25]]],minimum_chroma=.25,neutral_policy='include',inner=0,outer=.1),[[0.]])
        with self.assertRaises(OracleError):r.lab2000_range([[[.5]],[[0]],[[0]]],[.5,0,0],kL=0)
        COUNTS['ciede2000_decimal_pairs']=len(pairs)

    def test_invalid_schemas_and_capacity(self):
        bad=[lambda:r.threshold([[1]],dtype='uint8'),lambda:r.label_compact([[1]],connectivity=True),
             lambda:r.dilate([[1]],radius=True),lambda:r.dilate([[1]],radius=129),
             lambda:r.offset_discrete([[1]],radius=-1,sy=0),
             lambda:r.gaussian_feather([[1]],boundary='mirror'),
             lambda:r.apply_alpha([[[2]]],[[1]],alpha_channel=0),
             lambda:r.nearest_feature([[0]],output_dtype='bad'),
             lambda:r.thin_distance_ordered([[0]],output_dtype='bad'),
             lambda:r.multiply_mask([[[1]]],[[1]],channels='0',dtype='uint8')]
        for call in bad:
            with self.assertRaises(OracleError):call()

@unittest.skipUnless(EXTERNAL,'optional numpy/scipy/scikit-image not installed')
class ExternalTests(unittest.TestCase):
    def test_scipy_components_and_holes_3x3(self):
        for word in range(512):
            a=np.array([[word>>(3*y+x)&1 for x in range(3)] for y in range(3)],dtype=np.uint8)
            for conn in (4,8):
                structure=ndi.generate_binary_structure(2,1 if conn==4 else 2)
                labels,count=ndi.label(a,structure)
                self.assertEqual(r.label_compact(a.tolist(),connectivity=conn,dtype='float64'),labels.tolist())
                self.assertEqual(r.component_count(labels.tolist()),[int(count)])
                background=ndi.generate_binary_structure(2,2 if conn==4 else 1)
                holes=ndi.binary_fill_holes(a,structure=background).astype(int).tolist()
                self.assertEqual(r.fill_holes(a.tolist(),foreground_connectivity=conn,dtype='float64'),holes)
        COUNTS['scipy_component_hole_configurations']=1024

    def test_scipy_zero_lattice_morphology(self):
        rng=np.random.default_rng(182)
        for i in range(40):
            a=rng.integers(0,17,(3,4)).astype(float)/16
            for footprint in ('square','diamond','disk'):
                coords=r.footprint_offsets(footprint,1)
                kernel=np.zeros((3,3),dtype=bool)
                for y,x in coords:kernel[y+1,x+1]=True
                # Padding includes both halos; never per-stage crop at original canvas.
                padded=np.pad(a,3)
                ops={'dilate':ndi.grey_dilation,'erode':ndi.grey_erosion,
                     'opening':ndi.grey_opening,'closing':ndi.grey_closing}
                for name,fun in ops.items():
                    ref=fun(padded,footprint=kernel,mode='constant',cval=0)[3:-3,3:-3]
                    self.assertEqual(r.REGISTRY[name](a.tolist(),footprint=footprint,radius=1),ref.tolist())
        COUNTS['scipy_padded_morphology_comparisons']=480

    def test_scipy_edt_numerical_comparison(self):
        rng=np.random.default_rng(739)
        for i in range(80):
            a=rng.integers(0,2,(4,5),dtype=np.uint8);a[0,0]=1
            sy,sx=((1.,1.) if i%2 else (.75,2.))
            external=ndi.distance_transform_edt(a==0,sampling=(sy,sx))
            ours=r.nearest_feature(a.tolist(),sy=sy,sx=sx,exterior='none',dtype='float64')['distance']
            # External tolerance corroborates distances, not strict rounding or tie coordinates.
            np.testing.assert_allclose(ours,external,rtol=2e-15,atol=0)
        COUNTS['scipy_edt_arrays']=80

    def test_skimage_reconstruction(self):
        rng=np.random.default_rng(391)
        for i in range(40):
            a=rng.integers(0,17,(4,5)).astype(float)/16
            b=rng.integers(0,17,(4,5)).astype(float)/16
            low,high=np.minimum(a,b),np.maximum(a,b)
            for conn in (4,8):
                kernel=ndi.generate_binary_structure(2,1 if conn==4 else 2)
                for erosion in (False,True):
                    marker,limit=(high,low) if erosion else (low,high)
                    ref=skm.reconstruction(marker,limit,method='erosion' if erosion else 'dilation',footprint=kernel)
                    fun=r.reconstruct_erode if erosion else r.reconstruct_dilate
                    self.assertEqual(fun(marker.tolist(),limit.tolist(),connectivity=conn),ref.tolist())
        COUNTS['skimage_reconstruction_comparisons']=160

    def test_scipy_gaussian_cross_check(self):
        a=np.array([[0,.25,1],[1,.5,0]])
        for boundary,mode in [('zero','constant'),('replicate','nearest'),('reflect_half','reflect')]:
            out=r.gaussian_feather(a.tolist(),sigma_y=.5,sigma_x=2,radius_y=1,radius_x=2,boundary=boundary)
            ref=ndi.gaussian_filter(a,sigma=(.5,2),radius=(1,2),mode=mode,cval=0)
            np.testing.assert_allclose(out,ref,rtol=1e-14,atol=0)
        COUNTS['scipy_gaussian_arrays']=3


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--report',type=Path);args=parser.parse_args()
    start=time.perf_counter()
    suite=unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    versions={}
    for name in ('numpy','scipy','scikit-image'):
        try:versions[name]=importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:versions[name]=None
    with MPFR(128) as m:versions['MPFR']=m.version
    report=dict(schema_version=1,scope='independent mathematical oracle, NOT production conformance',
                python=sys.version.split()[0],platform=platform.platform(),dependencies=versions,
                tests_run=result.testsRun,failures=len(result.failures),errors=len(result.errors),
                skipped=len(result.skipped),passed=result.wasSuccessful(),
                elapsed_seconds=round(time.perf_counter()-start,3),scenario_counts=COUNTS,
                failure_details=[dict(test=str(t),trace=trace) for t,trace in result.failures+result.errors],
                untested=['production registration/API','Region demand and no-overread',
                          'TDM3/Result codecs and real ObjectIds','host budgets and cancellation',
                          'owner lifetime/cache/backend','production fenv preservation','signaling-NaN payload transfer'])
    if args.report:args.report.write_text(json.dumps(report,indent=2)+'\n')
    return 0 if result.wasSuccessful() else 1
if __name__=='__main__':sys.exit(main())
