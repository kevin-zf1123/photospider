"""Reproducible independent-oracle self-tests and machine-readable vectors.

These verify reference programs, not the retired or future Photospider runtime.
A vector tagged diagnostic or comparison_protocol must never become a strict golden.
"""
from __future__ import annotations
import importlib
import math
import random
import time
import traceback
from fractions import Fraction
from oracles import core as C,spatial as S,transcend as T,spectral as F,multiscale as M,restoration as R,comparison_protocol as E,special as X

MODULES={'core':C,'spatial':S,'transcend':T,'spectral':F,'multiscale':M,'restoration':R,'comparison_protocol':E}

def freeze(x,dtype='float64'):
    if isinstance(x,float):return {'dtype':dtype,'bits':C.hex_bits(x,dtype),'display':repr(x)}
    if isinstance(x,Fraction):return {'rational':[str(x.numerator),str(x.denominator)]}
    if isinstance(x,dict):return {str(k):freeze(v,dtype) for k,v in x.items()}
    if isinstance(x,(tuple,list)):return [freeze(v,dtype) for v in x]
    if isinstance(x,set):return [freeze(v,dtype) for v in sorted(x)]
    return x

def thaw(x):
    if isinstance(x,dict):
        if 'bits' in x and 'dtype' in x:return C.from_bits(int(x['bits'],16),x['dtype'])
        if set(x)=={'rational'}:return Fraction(*map(int,x['rational']))
        return {k:thaw(v) for k,v in x.items()}
    if isinstance(x,list):return [thaw(v) for v in x]
    return x

def require(condition,message='assertion failed'):
    if not condition:raise AssertionError(message)

def equal(actual,expected,dtype='float64'):
    if isinstance(actual,Fraction) and isinstance(expected,Fraction):
        require(actual==expected,f'{actual!r} != {expected!r}')
        return
    if isinstance(expected,Fraction):expected=C.rn(expected,dtype)
    if isinstance(expected,float):
        require(isinstance(actual,(int,float)),f'not scalar: {actual!r}')
        require(C.bits(float(actual),dtype)==C.bits(expected,dtype),f'bits differ: {actual!r} vs {expected!r}')
    elif isinstance(expected,(list,tuple)):
        require(len(actual)==len(expected),'length differs')
        for a,b in zip(actual,expected):equal(a,b,dtype)
    elif isinstance(expected,dict):
        for key,value in expected.items():
            require(key in actual,f'missing {key}');equal(actual[key],value,dtype)
    else:require(actual==expected,f'{actual!r} != {expected!r}')

class Suite:
    def __init__(self):self.cases=[];self.vectors=[]
    def case(self,name,ids,level,fn,check=None,expected=None,dtype='float64',request=None):
        start=time.perf_counter();row={'case_id':name,'members':ids,'proof_level':level,'dtype':dtype}
        try:
            result=fn()
            if expected is not None:equal(result,expected,dtype)
            if check is not None:check(result)
            row['status']='passed';row['actual']=freeze(result,dtype)
            if request is not None:
                self.vectors.append({'case_id':name,'members':ids,'proof_level':level,'dtype':dtype,
                                     'request':freeze(request), 'expected':freeze(result,dtype),
                                     'expected_origin':'independent reference output; replay validated against the case assertion in tests.py'})
        except Exception as exc:
            row['status']='failed';row['error']=f'{type(exc).__name__}: {exc}';row['traceback']=traceback.format_exc()
        row['seconds']=round(time.perf_counter()-start,6);self.cases.append(row)
    def vector(self,name,ids,function,args,kwargs,expected=None,check=None,dtype='float64',level='ExactRational'):
        mod,fn=function.split('.',1);call=getattr(MODULES[mod],fn)
        req={'function':function,'args':args,'kwargs':kwargs}
        self.case(name,ids,level,lambda:call(*args,**kwargs),check,expected,dtype,req)
    def rejects(self,name,ids,fn,exception=C.DomainError):
        def call():
            try:fn()
            except exception as e:return {'exception':type(e).__name__}
            raise AssertionError(f'expected {exception.__name__}')
        self.case(name,ids,'ContractFixture',call)
    def pending(self,name,ids,reason):
        self.cases.append({'case_id':name,'members':ids,'proof_level':'PinnedThirdPartyComparison','status':'pending','reason':reason,'seconds':0})


def run_all(include_diagnostics=True):
    suite=Suite();v=suite.vector;q=C.Q
    # Exact IEEE conversion: adversarial double rounding and range boundaries.
    for dtype,p,emin in [('float32',24,-126),('float64',53,-1022)]:
        suite.case(f'round_{dtype}_half_even',[],'ExactRational',lambda d=dtype,pp=p:C.rn(1+C.pow2(-pp),d),expected=1.0,dtype=dtype)
        suite.case(f'round_{dtype}_above_half',[],'ExactRational',lambda d=dtype,pp=p:C.rn(1+C.pow2(-pp)+C.pow2(-pp-70),d),expected=C.from_bits(C.bits(1.0,dtype)+1,dtype),dtype=dtype)
        suite.case(f'round_{dtype}_subnormal_half_negative',[],'ExactRational',lambda d=dtype,pp=p,e=emin:C.rn(-C.pow2(e-pp),d),expected=-0.0,dtype=dtype)
        suite.case(f'round_{dtype}_minsubnormal',[],'ExactRational',lambda d=dtype,pp=p,e=emin:C.rn(C.pow2(e-pp+1),d),expected=C.from_bits(1,dtype),dtype=dtype)
        emax=127 if dtype=='float32' else 1023
        suite.case(f'round_{dtype}_overflow',[],'ExactRational',lambda d=dtype,e=emax:C.rn(C.pow2(e+1),d),expected=float('inf'),dtype=dtype)
    suite.case('fp64_accelerated_uses_fp32_budget',[],'ContractFixture',lambda:C.accelerated_accept(1.0,1+4*2**-23),check=lambda x:require(x['accepted']))
    suite.case('fp64_accelerated_rejects_above_budget',[],'ContractFixture',lambda:C.accelerated_accept(1.0,1+5*2**-23),check=lambda x:require(not x['accepted']))
    suite.case('fp64_accelerated_small_requires_exact',[],'ContractFixture',lambda:C.accelerated_accept(2**-127,2**-127+2**-150),check=lambda x:require(not x['accepted']))
    suite.case('nan_payload_narrow',[],'ExactSpecial',lambda:C.round_bits(C.from_bits(0xfff0000000000001),'float32'),expected=0xffc00001)
    suite.case('zero_inf_product',['FIL-01D'],'ExactSpecial',lambda:X.fused_color([float('inf')],[0],[1]),check=lambda a:require(math.isnan(a[0]) and a[1]==0))
    suite.case('transparent_finite',['FIL-01D'],'ExactRational',lambda:X.fused_color([8,2],[0,0],[1,1]),expected=(0.,0.))
    suite.case('fused_coverage',['FIL-01D'],'ExactRational',lambda:X.fused_color([8,2],[0,1],[1,1]),expected=(2.,.5))
    suite.case('alpha_only_no_color',['FIL-01D'],'ExactRational',lambda:X.fused_color(None,[0,1],[1,1],alpha_only=True),expected=.5)
    suite.case('reduction_nan_precedes_generated',['FIL-01D'],'ExactSpecial',lambda:C.bits(X.sum_products([(0,float('inf')),(C.from_bits(0xfff8000000000123),)])),expected=0xfff8000000000123)
    suite.case('convolution_overflow',['FIL-01A'],'ExactRational',lambda:S.convolve2d([[C.from_bits(0x7f7fffff,'float32')]],[[2]],dtype='float32'),expected=[[float('inf')]],dtype='float32')
    suite.case('rank_opposite_infinities',['FIL-05B'],'ExactSpecial',lambda:X.quantile([-float('inf'),float('inf')],q(1,2),'linear','float64'),check=lambda a:require(math.isnan(a)))
    suite.case('rank_same_infinities',['FIL-05B'],'ExactSpecial',lambda:X.quantile([float('inf'),float('inf')],q(1,2),'linear','float64'),expected=float('inf'))
    def diffusion_rounds():
        kw=dict(kappa=1,dt=.2,dtype='float32')
        one=R.diffusion([[0.,1.]],steps=1,**kw)
        return R.diffusion([[0.,1.]],steps=2,**kw),R.diffusion(one,steps=1,**kw)
    suite.case('diffusion_float32_stages',['RES-06B'],'ExactRationalStaged',diffusion_rounds,check=lambda a:equal(a[0],a[1],'float32'))
    suite.case('blind_psf_direct_float32',['RES-10A'],'ExactRationalStaged',lambda:R.blind_psf([[1.,3.]],[[1.,2.]],[[.5,.5]],[[1,1]],step_image=.1,step_psf=.01,dtype='float32')[1],check=lambda a:require(all(v==C.rn(C.frac(v),'float32') for row in a for v in row)),dtype='float32')
    for dtype in ('float32','float64'):
        suite.case('convolution_negative_zero_'+dtype,['FIL-01A'],'ExactSpecial',lambda d=dtype:S.convolve2d([[-0.]],[[1]],bias=-0.,dtype=d),expected=[[-0.]],dtype=dtype)
        suite.case('convolution_negative_denominator_zero_'+dtype,['FIL-01A'],'ExactSpecial',lambda d=dtype:S.convolve2d([[-0.]],[[1,-2]],boundary='clamp',normalization='sum',bias=-0.,dtype=d),expected=[[-0.]],dtype=dtype)
        suite.case('rl_nan_zero_prediction_'+dtype,['RES-09A'],'ExactSpecial',lambda d=dtype:X.rl_ratio(C.from_bits(0xfff8000000000123),1,0,0,dtype=d),expected=C.rn(C.from_bits(0xfff8000000000123),dtype),dtype=dtype)
        suite.case('simplex_nonfinite_'+dtype,['RES-10A'],'ExactSpecial',lambda d=dtype:X.exceptional_simplex([float('inf'),0],dtype=d),check=lambda a:require(all(math.isnan(v) for v in a)),dtype=dtype)
    for norm,impulse_value,dc in [('backward',1.,8.),('ortho',.5,4.)]:
        suite.case('dft_normalized_impulse_'+norm,['FRQ-01A'],'DirectedMPFR',lambda n=norm:F.dft2([[1,0],[0,0]],norm=n),expected=([[impulse_value]*2 for _ in range(2)],[[0.]*2 for _ in range(2)]))
        suite.case('dft_normalized_constant_'+norm,['FRQ-01A'],'DirectedMPFR',lambda n=norm:F.dft2([[2,2],[2,2]],norm=n),expected=([[dc,0.],[0.,0.]],[[0.]*2 for _ in range(2)]))
    suite.case('blind_zero_step_psf_float32',['RES-10A'],'ExactRational',lambda:R.blind_psf([[1]],[[1]],[[.1,.9]],[[1,1]],iterations=0,dtype='float32')[1],check=lambda a:require(all(v==C.rn(C.frac(v),'float32') for row in a for v in row)),dtype='float32')
    def guided_dehaze_float32():
        guide=[[C.rn(q(7,3),'float32'),C.rn(q(14,3),'float32')]]
        require(C.bits(guide[0][0],'float32')==0x40155555)
        a,b=map(C.frac,guide[0]);mean=(a+b)/2;delta=b-a
        slope=C.frac(C.rn((delta/8)/(delta*delta/4+1)))
        intercept=C.frac(C.rn(q(1,2)-slope*mean))
        expected=[[C.rn(slope*g+intercept,'float32') for g in (a,b)]]
        result=S.guided([[.25,.75]],guide,0,1,epsilon=1,dtype='float32')
        equal(result,expected,'float32')
        return result
    suite.case('dehaze_guided_float32_stages',['RES-14D'],'ExactRationalStaged',guided_dehaze_float32,dtype='float32')
    # Boundary words are replaced with explicit short-array behavior.
    for mode,expected in [('reflect_half',[1,0,0,1,2,2,1]),('reflect_whole',[2,1,0,1,2,1,0]),('wrap',[1,2,0,1,2,0,1]),('clamp',[0,0,0,1,2,2,2])]:
        suite.case('boundary_'+mode,['FIL-01A','FIL-01B'],'ExactRational',lambda mode=mode:[C.map_index(i,3,mode) for i in range(-2,5)],expected=expected)
    suite.case('single_pixel_reflection',['FIL-01A'],'ExactRational',lambda:[C.map_index(-10**9,1,m) for m in ('reflect_half','reflect_whole','wrap','clamp')],expected=[0,0,0,0])
    for dtype in ('float32','float64'):
        for direction,id_,expected in [('convolve','FIL-01A',[[1,4,8]]),('correlate','FIL-01B',[[5,10,4]])]:
            v('asymmetric_'+direction+'_'+dtype,[id_],'spatial.convolve2d',[[[1,2,4]],[[1,2]]],{'boundary':'constant','direction':direction,'dtype':dtype},expected,dtype=dtype)
        v('normalized_mask_poison_'+dtype,['FIL-01C'],'spatial.normalized_convolution',[[[1,float('nan'),5]],[[1,0,1]],[[1,1,1]]],{'anchor':(0,1),'boundary':'truncate','dtype':dtype},([[1,3,5]],[[1,1,1]]),dtype=dtype)
        for mean,id_,expected in [(False,'FIL-03A',[[18.0]]),(True,'FIL-03B',[[2.0]])]:
            v('box_'+str(mean)+'_'+dtype,[id_],'spatial.box',[[[2.0]],3,3,(1,1)],{'mean':mean,'dtype':dtype},expected,dtype=dtype)
        v('percentile_linear_'+dtype,['FIL-05B'],'spatial.percentile',[[[0,10]],[[1,1]],(0,0)],{'q':0.25,'boundary':'clamp','dtype':dtype},[[2.5,10.0]],dtype=dtype)
        v('detail_gain_'+dtype,['FIL-16B'],'restoration.detail_gain',[[[2]],[[3]]],{'gain':2,'dtype':dtype},[[8]],dtype=dtype)
    v('convolution_full',['FIL-01A','FRQ-07A'],'spatial.convolve2d',[[[1,2,4]],[[1,2]]],{'anchor':(0,1),'boundary':'constant','output_shape':'full'},[[1,4,8,8]])
    suite.case('convolution_origins',['FIL-01A','FIL-01B','FRQ-07A'],'ContractFixture',lambda:[S.convolution_geometry(1,3,1,2,(0,1),'full',d) for d in ('convolve','correlate')],expected=[((1,4),(0,-1)),((1,4),(0,0))])
    suite.rejects('valid_empty_rejected',['FIL-01A'],lambda:S.convolve2d([[1]],[[1,2]],output_shape='valid',boundary='constant'))
    suite.rejects('zero_sum_normalizer',['FIL-01A'],lambda:S.convolve2d([[1]],[[1,-1]],normalization='sum'))
    v('zero_tap_no_poison_read',['FIL-01A'],'spatial.convolve2d',[[[float('nan')]],[[0]]],{},[[0.0]])
    suite.case('sparse_support_no_bbox',['FIL-01A'],'ContractFixture',lambda:S.support_for_output((1,5),[[1,0,1]],(0,1),(0,2)),expected={(0,1),(0,3)})
    for direction,id_,expected in [('convolve','FIL-02A',[[1,4,8]]),('correlate','FIL-02B',[[5,10,4]])]:
        v('separable_'+direction,[id_],'spatial.separable',[[[1,2,4]],[1,2],[1]],{'direction':direction,'boundary':'constant'},expected)
    v('median_mean',['FIL-05A'],'spatial.percentile',[[[1,2,9,10]],[[1,1,1,1]],(0,0)],{'boundary':'clamp'},[[5.5,9.5,10,10]])
    for method,out in [('lower',2),('higher',9),('nearest_even',9),('midpoint',5.5)]:
        suite.case('median_even_'+method,['FIL-05A','FIL-05B'],'ExactRational',lambda method=method:S.percentile([[1,2,9,10]],[[1,1,1,1]],(0,0),interpolation=method,boundary='clamp')[0][0],expected=out)
    suite.case('rank_signed_zero_copy',['FIL-05A','FIL-05B'],'ExactRational',lambda:S.percentile([[-0.0,0.0]],[[1,1]],(0,0),q=0,interpolation='lower',boundary='clamp')[0][0],expected=-0.0)
    suite.case('rank_nan_propagation',['FIL-05A'],'ExactSpecial',lambda:S.percentile([[1,float('nan')]],[[1,1]],(0,0)),check=lambda a:require(all(math.isnan(v) for row in a for v in row)))
    v('gaussian_coefficients',['FIL-04A'],'transcend.gaussian_kernel',[1,1],{},[C.from_bits(0x3fe368b2fc6f960a),1.0,C.from_bits(0x3fe368b2fc6f960a)],level='DirectedMPFR')
    v('gaussian_zero_sigma',['FIL-04A'],'transcend.gaussian_kernel',[0,0],{},[1.0],level='DirectedMPFR')
    suite.rejects('gaussian_zero_sigma_nonzero_radius',['FIL-04A'],lambda:T.gaussian_kernel(0,1))
    suite.case('gaussian_constant',['FIL-04B'],'DirectedMPFR+ExactRational',lambda:S.separable([[3.0]*3]*2,T.gaussian_kernel(1,1),T.gaussian_kernel(1,1),(1,1),normalization='sum'),expected=[[3.0]*3]*2)
    suite.case('bilateral_symmetric',['FIL-06A'],'DirectedMPFR+ExactRational',lambda:T.bilateral([[1,2,3]],radius_y=0,radius_x=1)[0][1],expected=2.0)
    suite.case('joint_bilateral_symmetric',['FIL-06B'],'DirectedMPFR+ExactRational',lambda:T.bilateral([[1,2,3]],[[[0,0],[0,0],[0,0]]],radius_y=0,radius_x=1,metric_scale=[1,1])[0][1],expected=2.0)
    mid=C.frac(C.rn(q(7,3)));expected_guided=[[C.rn((q(3,2)+mid)/2),C.rn((q(3,2)+mid+3)/3),C.rn((mid+3)/2)]]
    v('guided_scalar_two_windows',['FIL-07A'],'spatial.guided',[[[1,2,4]],[[0,0,0]],0,1],{'epsilon':1},expected_guided,level='ExactRationalStaged')
    v('guided_vector_two_windows',['FIL-07B'],'spatial.guided',[[[1,2,4]],[[[0,0],[0,0],[0,0]]],0,1],{'epsilon':1},expected_guided,level='ExactRationalStaged')
    suite.case('guided_2r_counterexample',['FIL-07A'],'ContractFixture',lambda:S.guided([[0,0,0,0,1,0,0]],[[0]*7],0,1,epsilon=1)[0][2],check=lambda x:require(x>0))
    ramp=[[float(x) for x in range(5)] for y in range(5)]
    for method,id_ in [('central','FIL-08A'),('sobel','FIL-08B'),('scharr','FIL-08C')]:
        suite.case('gradient_ramp_'+method,[id_],'ExactRational',lambda method=method:tuple(a[2][2] for a in S.gradient(ramp,method)),expected=(1.0,0.0))
    v('magnitude_3_4',['FIL-09A'],'transcend.magnitude',[[[3]],[[4]]],{},[[5.0]],level='DirectedMPFR')
    v('orientation_turn',['FIL-09B'],'transcend.orientation',[[[-1,0,0]],[[0,1,0]]],{'unit':'turn'},([[0.5,0.25,0.0]],[[1,1,0]]),level='DirectedMPFR')
    parabola=[[x*x+y*y for x in range(5)] for y in range(5)]
    for eight,id_ in [(False,'FIL-10A'),(True,'FIL-10B')]:
        suite.case('laplacian_quadratic_'+str(eight),[id_],'ExactRational',lambda eight=eight:S.laplacian(parabola,eight=eight)[2][2],expected=4.0)
    suite.case('log_center',['FIL-11A'],'DirectedMPFR',lambda:T.log_kernel(1,1)[0][1][1],expected=C.from_bits(0xbfd45f306dc9c883))
    suite.case('log_constant_zero_dc',['FIL-11B'],'DirectedMPFR+ExactRational',lambda:S.zero_dc_filter([[2.0]*3]*3,T.log_kernel(1,1)[0]),expected=[[0.0]*3]*3)
    suite.case('dog_constant',['FIL-11C'],'StagedReference',lambda:[[C.rn(C.frac(a)-C.frac(b)) for a,b in zip(ra,rb)] for ra,rb in zip(S.separable([[3]*3],T.gaussian_kernel(1,1),[1],(0,1),normalization='sum'),S.separable([[3]*3],T.gaussian_kernel(2,2),[1],(0,2),normalization='sum'))],expected=[[0.0]*3])
    im=[[x*x+2*x*y+3*y*y for x in range(5)] for y in range(5)]
    suite.case('hessian_polynomial',['FIL-12A'],'ExactRational',lambda:tuple(a[2][2] for a in S.hessian(im)),expected=(2.0,2.0,6.0))
    v('structure_tensor_constant_gradient',['FIL-12B'],'spatial.structure_tensor',[[[2]],[[3]],3,3,(1,1)],{},([[4]],[[6]],[[9]]))
    v('symmetric_eigen',['FIL-12C'],'transcend.eigen2d',[[[4]],[[0]],[[1]]],{},([[4]],[[1]],[[0.0]],[[1]]),level='DirectedMPFR')
    v('canny_constant',['FIL-13A'],'spatial.canny',[[[1]*5 for _ in range(5)]],{'sigma':1,'radius':1,'low':0.125,'high':0.25},[[0]*5 for _ in range(5)],level='StagedReference')
    v('hysteresis_long_chain',['FIL-13B'],'spatial.hysteresis',[[[1]*70+[0,1]],[[1]+[0]*71]],{},[[1]*70+[0,0]],level='DiscreteExact')
    suite.rejects('hysteresis_strong_outside_weak',['FIL-13B'],lambda:S.hysteresis([[0]],[[1]]))
    suite.case('gabor_zero_frequency',['FIL-14A'],'DirectedMPFR',lambda:T.gabor_kernel(frequency=0,radius_y=0,radius_x=0),expected=([[1.0]],[[0.0]]))
    suite.case('gabor_complex_response',['FIL-14B'],'ExactRational',lambda:(S.convolve2d([[1,2,4]],[[1,2]],direction='correlate',boundary='constant'),S.convolve2d([[1,2,4]],[[3,4]],direction='correlate',boundary='constant')),expected=([[5,10,4]],[[11,22,12]]))
    suite.case('gabor_bank_two_members',['FIL-14C'],'StagedReference',lambda:[T.gabor_kernel(frequency=f,radius_y=0,radius_x=0) for f in (0,0.25)],expected=[([[1.0]],[[0.0]])]*2)
    constant=[[3.0]*5 for _ in range(3)]
    suite.case('gaussian_pyramid_constant',['FIL-15A'],'ExactRationalStaged',lambda:[a['values'] for a in M.pyramid(constant,2,mode='gaussian')['bands']],expected=[constant,[[3.0]*3]*2,[[3.0]*2]])
    suite.case('laplacian_pyramid_zero_details',['FIL-15B'],'ExactRationalStaged',lambda:[a['values'] for a in M.pyramid(constant,2)['bands'][:-1]],expected=[[[0.0]*5]*3,[[0.0]*3]*2])
    integer_image=[[float(1+y*5+x) for x in range(5)] for y in range(3)]
    suite.case('laplacian_pyramid_odd_roundtrip',['FIL-15B','FIL-15C'],'ExactRationalStaged',lambda:M.pyramid(mode='reconstruct',bands=M.pyramid(integer_image,2)),expected=integer_image)
    nan=C.from_bits(0x7ff8000000001234)
    v('unsharp_disabled_preserves_bits',['FIL-16A'],'restoration.unsharp',[[[-0.0,nan]]],{'amount':0},[[-0.0,nan]],level='BitwiseCopy')
    v('local_contrast_equal_base',['FIL-17A'],'transcend.local_contrast',[[[3]],[[3]]],{'amount':1,'detail_scale':1},[[3]],level='DirectedMPFR')
    v('local_laplacian_identity',['FIL-17B'],'multiscale.local_laplacian',[[[-0.0,nan]]],{'detail_exponent':1,'edge_slope':1},[[-0.0,nan]],level='BitwiseCopy')
    suite.case('local_laplacian_actual_remap_constant',['FIL-17B'],'StagedReference',lambda:M.local_laplacian([[2.0]*2]*2,levels=1,detail_exponent=2,edge_slope=0),expected=[[2.0]*2]*2)
    impulse=[[0,0,0],[0,1,0],[0,0,0]]
    for disk,id_,den in [(False,'FIL-18A',9),(True,'FIL-18B',5)]:
        suite.case('variable_footprint_'+str(disk),[id_],'ExactRational',lambda disk=disk:S.variable_gather(impulse,1,disk=disk,max_radius=1)[0][1][1],expected=q(1,den))
    v('post_aa_flat_guide_copy',['FIL-19A'],'spatial.post_aa',[[[-0.0,2.0]],[[0,0]]],{'threshold':1},[[-0.0,2.0]],level='BitwiseCopy')
    suite.case('local_variance_population',['FIL-20A'],'ExactRational',lambda:S.local_moments([[1,2,3]],1,3,(0,1))[1][0][1],expected=q(2,3))
    suite.case('local_variance_unbiased',['FIL-20A'],'ExactRational',lambda:S.local_moments([[1,2,3]],1,3,(0,1),ddof=1)[1][0][1],expected=1.0)
    suite.case('local_covariance_negative',['FIL-20B'],'ExactRational',lambda:S.local_moments([[1,2,3]],1,3,(0,1),other=[[-1,-2,-3]])[1][0][1],expected=-q(2,3))
    v('straight_alpha_zero_hidden_color',['FIL-04B'],'spatial.straight_positive_blur',[[8,2],[0,1],[1,1]],{},(2.0,0.5))
    v('straight_all_zero_alpha',['FIL-04B'],'spatial.straight_positive_blur',[[8,2],[0,0],[1,1]],{},(0.0,0.0))
    v('straight_alpha_underflow_hidden_color',['FIL-04B'],'spatial.straight_positive_blur',[[8,2],[C.from_bits(1,'float32'),0],[1,1]],{'dtype':'float32'},(8.0,0.0),dtype='float32')
    # Frequency transforms: roots-of-unity exact zeros are essential.
    for h,w in [(1,1),(1,5),(2,3),(3,2),(4,4)]:
        imp=[[float(y==0 and x==0) for x in range(w)] for y in range(h)]
        v(f'dft_impulse_{h}_{w}',['FRQ-01A'],'spectral.dft2',[imp],{},([[1.0]*w for _ in range(h)],[[0.0]*w for _ in range(h)]),level='Cyclotomic+DirectedMPFR')
        v(f'idft_ones_{h}_{w}',['FRQ-01B'],'spectral.dft2',[[[1.0]*w for _ in range(h)]],{'inverse':True},(imp,[[0.0]*w for _ in range(h)]),level='Cyclotomic+DirectedMPFR')
    v('dft_constant_odd_exact_zeros',['FRQ-01A'],'spectral.dft2',[[[1]*5]],{},([[5.0,0,0,0,0]],[[0.0]*5]),level='Cyclotomic+DirectedMPFR')
    v('rfft_odd_width',['FRQ-02A'],'spectral.rfft2',[[[1,0,0,0,0]]],{},([[1,1,1]],[[0.0]*3]),level='Cyclotomic+DirectedMPFR')
    v('irfft_odd_width',['FRQ-02B'],'spectral.irfft2',[[[1,1,1]],[[0,0,0]],5],{},[[1.0,0.0,0.0,0.0,0.0]],level='Cyclotomic+DirectedMPFR')
    suite.case('rfft_boundary_column_nonzero_imaginary',['FRQ-02A','FRQ-02B'],'Cyclotomic+DirectedMPFR',lambda:F.rfft2([[0,1],[1,0],[2,3]])[1],check=lambda a:require(a[1][0]!=0 and a[2][0]==-a[1][0]))
    suite.rejects('irfft_invalid_boundary_conjugacy',['FRQ-02B'],lambda:F.irfft2([[1],[1],[1]],[[0],[1],[1]],1))
    v('hermitian_self_copy_signed_zero',['FRQ-02C'],'spectral.project_hermitian',[[[-0.0]],[[2.0]]],{},([[-0.0]],[[0.0]]),level='ExactRational+Copy')
    v('fftshift_odd',['FRQ-03A'],'spectral.shift',[[[0,1,2,3,4]]],{},[[3,4,0,1,2]],level='BitwiseCopy')
    v('ifftshift_odd',['FRQ-03B'],'spectral.shift',[[[3,4,0,1,2]]],{'inverse':True},[[0,1,2,3,4]],level='BitwiseCopy')
    v('hann_periodic',['FRQ-04A'],'spectral.window',[1,4],{'sampling':'periodic'},[[0.0,0.5,1.0,0.5]],level='DirectedMPFR')
    v('hann_symmetric',['FRQ-04A'],'spectral.window',[1,3],{},[[0.0,1.0,0.0]],level='DirectedMPFR')
    v('apply_window',['FRQ-04B'],'spectral.apply_window',[[[2,2,2]],[[0,1,0]]],{},[[0.0,2.0,0.0]])
    v('window_statistics',['FRQ-04C'],'spectral.window_stats',[[[0,1,0]]],{},(q(1,3),q(1,3),3))
    v('ideal_lowpass',['FRQ-05A'],'spectral.radial_response',[1,4],{'cutoff':0.25,'kind':'ideal'},([[1,1,0,1]],[[0.0]*4]),level='ExactDiscrete')
    v('ideal_bandpass',['FRQ-05B'],'spectral.band_response',[1,4],{'cutoff_low':0.125,'cutoff_high':0.25,'kind':'ideal'},([[0,1,0,1]],[[0.0]*4]),level='ExactDiscrete')
    suite.case('butterworth_amplitude_cutoff',['FRQ-05A'],'DirectedMPFR',lambda:F.radial_response(1,4,cutoff=.25,kind='butterworth')[0][0][1],expected=C.from_bits(0x3fe6a09e667f3bcd))
    suite.case('paired_notch_zero_at_both_centres',['FRQ-05C'],'DirectedMPFR',lambda:F.notch_response(1,4,centers=[(0,.25)],sigma_y=1,sigma_x=.125)[0],check=lambda a:require(a[0][1]==0 and a[0][3]==0))
    suite.rejects('duplicate_notch_conjugate_orbit',['FRQ-05C'],lambda:F.notch_response(1,4,centers=[(0,.25),(0,-.25)],sigma_y=1,sigma_x=.125))
    v('complex_frequency_multiply',['FRQ-06A'],'spectral.multiply',[[[1]],[[2]],[[3]],[[4]]],{},([[-5]],[[10]]))
    v('circular_kernel_folding',['FRQ-07B'],'spatial.convolve2d',[[[1,2]],[[1,2,3]]],{'boundary':'wrap'},[[8,10]])
    for save,id_ in [(False,'FRQ-08A'),(True,'FRQ-08B')]:
        def check_partition(blocks):
            counts=[[0]*7 for _ in range(5)]
            for b in blocks:
                y,x,h,w=b['payload']
                for yy in range(y,y+h):
                    for xx in range(x,x+w):counts[yy][xx]+=1
            require(all(v==1 for row in counts for v in row),'payload gaps or duplicates')
        v('block_partition_'+str(save),[id_],'spectral.block_partition',[5,7,2,3,3,4],{'save':save},check=check_partition,level='ContractFixture')
    v('dct_I_2x2',['FRQ-09A'],'spectral.dct2',[[[1,2],[3,4]]],{'kind':'I'},[[10,-2],[-4,0]],level='Cyclotomic+DirectedMPFR')
    v('idct_I_2x2',['FRQ-09B'],'spectral.dct2',[[[10,-2],[-4,0]]],{'kind':'I','inverse':True},[[1,2],[3,4]],level='Cyclotomic+DirectedMPFR')
    for kind in ('II','III','IV'):
        for inverse in (False,True):
            v(f'dct_ortho_single_{kind}_{inverse}',['FRQ-09B' if inverse else 'FRQ-09A'],'spectral.dct2',[[[3]]],{'kind':kind,'norm':'ortho','inverse':inverse},[[3.0]],level='Cyclotomic+DirectedMPFR')
    suite.rejects('dct_I_single_invalid',['FRQ-09A'],lambda:F.dct2([[1]],kind='I'))
    for profile,a_id,b_id in zip(M.PROFILES,['FRQ-10A','FRQ-10C','FRQ-10E'],['FRQ-10B','FRQ-10D','FRQ-10F']):
        suite.case('wavelet_roundtrip_'+profile,[a_id,b_id],'ExactRationalStaged',lambda profile=profile:M.wavelet(inverse=True,bands=M.wavelet(integer_image,1,profile=profile)),expected=integer_image)
        if profile!='stationary_haar_mean_v1':
            suite.case('wavelet_basis_'+profile,[a_id],'ExactRationalStaged',lambda profile=profile:{b['role']:b['values'] for b in M.wavelet([[1,2],[3,4]],1,profile=profile)['bands']},expected={'LL':[[2.5]],'LH_y':[[2.0]],'HL_x':[[1.0]],'HH':[[0.0]]})
    # Restoration references and explicit staged solvers.
    v('noise_mad_constant',['RES-01A'],'restoration.noise_mad',[[[2,2],[2,2]],[[1,1],[1,1]]],{},0.0,level='ExactRationalStaged')
    suite.case('noise_mad_quantile_constant',['RES-01A'],'DirectedMPFR',lambda:R.noise_mad([[0,1,0],[1,0,1],[0,1,0]],[[1]*3]*3),check=lambda x:require(1.4826<x<1.4827))
    v('calibrated_variance',['RES-01B'],'restoration.calibrated_variance',[[[9]]],{'gain':2,'offset':1,'read_sigma':0},([[16]],[[4]]),level='ExactRational+DirectedMPFR')
    for corrected,id_ in [(False,'RES-02A'),(True,'RES-02B')]:
        kw={'patch_radius':0,'search_radius':1,'h_parameter':1}
        if corrected:kw['noise_sigma']=0
        v('nlm_constant_guide_'+str(corrected),[id_],'restoration.nlm',[[[1,2,4]],[[0,0,0]]],kw,[[1.5,q(7,3),3]],level='DirectedMPFR+ExactRational')
    bands=M.wavelet([[1,2],[3,4]],1)
    suite.case('wavelet_threshold_soft',['RES-03A'],'ExactRationalStaged',lambda:{b['role']:b['values'] for b in M.threshold_bands(bands,[[1,1,1]])['bands']},expected={'LL':[[2.5]],'LH_y':[[1.0]],'HL_x':[[0.0]],'HH':[[0.0]]})
    v('cycle_spin_constant',['RES-03B'],'multiscale.cycle_spin',[[[2]*2]*2,[[1,1,1]]],{'levels':1,'shifts':[(0,0),(0,1)]},[[2]*2]*2,level='ExactRationalStaged')
    for iso,id_ in [(True,'RES-05A'),(False,'RES-05B')]:
        v('tv_one_step_'+str(iso),[id_],'restoration.tv_cp',[[[0,1]]],{'lam':1,'iterations':1,'tau':.25,'sigma':.25,'isotropic':iso},[[q(1,20),q(19,20)]],level='ExactRationalStaged')
    v('diffusion_reciprocal_one_step',['RES-06B'],'restoration.diffusion',[[[0,1]]],{'kappa':1,'dt':.25,'steps':1},[[.125,.875]],level='ExactRationalStaged')
    ev=C.frac(C.from_bits(0x3fd78b56362cef38))
    v('diffusion_exponential_one_step',['RES-06A'],'restoration.diffusion',[[[0,1]]],{'kappa':1,'dt':.25,'steps':1,'exponential':True},[[C.rn(ev/4),C.rn(1-ev/4)]],level='DirectedMPFRStaged')
    v('anscombe_forward_zero',['RES-07A'],'restoration.anscombe_forward',[[[0]]],{},[[C.from_bits(0x3ff3988e1409212e)]],level='DirectedMPFR')
    v('anscombe_algebraic',['RES-07B'],'restoration.anscombe_algebraic',[[[2]]],{},[[.625]])
    v('generalized_forward_negative_observation',['RES-07D'],'restoration.generalized_anscombe_forward',[[[-100]]],{'gain':1,'offset':0,'read_sigma':1},[[0.0]],level='DirectedMPFR')
    v('generalized_algebraic_returns_offset',['RES-07E'],'restoration.generalized_anscombe_algebraic',[[[0]]],{'gain':2,'offset':10,'read_sigma':1},[[10.0]])
    if include_diagnostics:
        v('anscombe_mean_inverse_diagnostic',['RES-07C'],'restoration.anscombe_mean_inverse',[2],{'dps':45},check=lambda x:require(x['proof_level']=='HighPrecisionDiagnostic' and .78169<x['value']<.78170),level='HighPrecisionDiagnostic')
        v('generalized_mean_inverse_positive_readnoise_diagnostic',['RES-07F'],'restoration.generalized_mean_inverse',[0],{'gain':2,'offset':3,'read_sigma':1,'dps':25},check=lambda x:require(x['value']==3 and not x['quadrature_certificate']),level='HighPrecisionDiagnostic')
    v('wiener_delta',['RES-08A'],'restoration.wiener',[[[2,4]],[[1]]],{'nsr':1},[[1,2]])
    v('tikhonov_gradient_constant',['RES-08B'],'restoration.tikhonov',[[[3]*2]*2,[[1]]],{'lam':1,'regularizer':'gradient'},[[3]*2]*2)
    v('wiener_singular_minimum_norm',['RES-08A'],'restoration.wiener',[[[1,3]],[[1,1]]],{'nsr':0,'singular':'zero'},[[2,2]])
    suite.rejects('wiener_singular_error',['RES-08A'],lambda:R.wiener([[1,3]],[[1,1]],nsr=0))
    v('rl_delta_one_step',['RES-09A'],'restoration.rl',[[[2,4]],[[1]],[[1,1]],[[0,0]],[[1,1]]],{},([[2,4]],[[0,0]]),level='ExactRationalStaged')
    v('rl_epsilon_one_step',['RES-09B'],'restoration.rl',[[[2,4]],[[1]],[[1,1]],[[0,0]],[[1,1]]],{'epsilon':1},([[1,2]],[[0,0]]),level='ExactRationalStaged')
    v('rl_masked_nonfinite_observation',['RES-09A'],'restoration.rl',[[[nan,nan]],[[1]],[[2,3]],[[0,0]],[[0,0]]],{},([[2,3]],[[1,1]]),level='ExactRationalStaged')
    suite.case('rl_true_adjoint_dot',['RES-09A','RES-09B'],'ExactRational',lambda:R.forward_adjoint([[1,2,3]],[[1,2]],probe=[[2,4,8]]),check=lambda r:require(r['dot_forward']==r['dot_adjoint']==q(74,3)))
    suite.rejects('rl_zero_prediction_positive_observation',['RES-09A'],lambda:R.rl([[1]],[[1]],[[0]],[[0]],[[1]]))
    v('simplex_exact_projection',['RES-10A'],'restoration.simplex_projection',[[q(1,5),-q(1,10),q(9,10)]],{},[q(3,20),0,q(17,20)],level='ExactRationalInternalState')
    v('blind_psf_fixed_point',['RES-10A'],'restoration.blind_psf',[[[1,2]],[[1,2]],[[1]],[[1]]],{'iterations':1,'step_image':.25,'step_psf':.25},([[1,2]],[[1]]),level='ExactRationalStaged')
    suite.case('deband_flat_range',['RES-11A'],'ExactRational',lambda:R.low_contrast_smoothing([[0,1,0]],[[1,1,1]],range_threshold=1,strength=.5)[0][1],expected=q(2,3))
    v('deband_mask_zero_copy',['RES-11A'],'restoration.low_contrast_smoothing',[[[nan]],[[0]]],{},[[nan]],level='BitwiseCopy')
    v('deblock_known_boundary',['RES-11B'],'restoration.pairwise_block_boundary_smoothing',[[[0,1]],[[1,1]]],{'block_width':2,'block_height':2,'origin_x':1,'threshold':1,'strength':.25},[[.25,.75]],level='ExactRationalStaged')
    v('deblock_mask_zero_bitcopy',['RES-11B'],'restoration.pairwise_block_boundary_smoothing',[[[-0.0,1]],[[0,1]]],{'block_width':2,'block_height':2,'origin_x':1},[[-0.0,1]],level='BitwiseCopy')
    suite.case('notch_workflow_zero_depth_impulse',['RES-12A'],'StagedReference',lambda:F.irfft2(*F.multiply(*F.rfft2([[1,0,0,0]]),[[1,1,1]],[[0,0,0]]),4),expected=[[1.0,0.0,0.0,0.0]])
    suite.case('gaussian_descreen_constant',['RES-12B'],'StagedReference',lambda:S.separable([[2,2,2]],T.gaussian_kernel(1,1),[1],(0,1),normalization='sum'),expected=[[2,2,2]])
    v('nonflat_ball_constant',['RES-13A'],'restoration.ball_background',[[[2]*3]*3],{'radius':1,'height':1},[[2]*3]*3,level='DirectedMPFRStaged')
    v('background_subtract_signed',['RES-13B'],'restoration.subtract_background',[[[1]],[[3]]],{},[[-2]])
    v('flat_field_known_reference',['RES-13C'],'restoration.flat_field',[[[10]],[[2]],[[6]],[[2]],[[1]]],{'reference_gain':4},([[8]],[[1]]))
    v('flat_field_mask_zero_skips_calibration',['RES-13C'],'restoration.flat_field',[[[-0.0]],[[nan]],[[nan]],[[nan]],[[0]]],{'reference_gain':1,'invalid':'copy_input'},([[-0.0]],[[0]]),level='BitwiseCopy')
    rgb=[[[.5,.75,1.0],[.5,.75,1.0]]]
    v('airlight_select_first_tie',['RES-14A'],'restoration.airlight',[rgb],{'radius':0,'top_fraction':1},([[.5,.5]],[.5,.75,1.0],(0,0)),level='DiscreteExact')
    v('transmission_explicit_omega',['RES-14B'],'restoration.transmission',[rgb,[.5,.75,1.0]],{'radius':0,'omega':.5},[[.5,.5]])
    v('apply_dehaze_analytic',['RES-14C'],'restoration.apply_dehaze',[[[[.75,.75,.75]]],[1,1,1],[[.5]]],{'t_floor':.125},[[[.5,.5,.5]]])
    suite.case('dehaze_workflow_constant',['RES-14D'],'StagedReference',lambda:R.apply_dehaze(rgb,R.airlight(rgb,radius=0)[1],R.transmission(rgb,R.airlight(rgb,radius=0)[1],radius=0,omega=.5)),expected=rgb)
    suite.rejects('airlight_black_fails_only_when_requested',['RES-14A'],lambda:R.airlight([[[0,0,0]]],radius=0))
    v('dark_channel_black_without_airlight',['RES-14A'],'restoration.airlight',[[[[0,0,0]]]],{'radius':0,'request_airlight':False},([[0.0]],None,None),level='DiscreteExact')
    # Native algorithms: a manifest does not execute a third-party comparison.
    for engine,id_ in [('smaa','FIL-19B'),('bm3d','RES-04A'),('cbm3d','RES-04B')]:
        v('manifest_missing_'+engine,[id_],'comparison_protocol.'+engine,[{}],{},check=lambda x:require(not x['manifest_complete'] and not x['comparison_executed']),level='ContractFixture')
        suite.pending('real_golden_'+engine,[id_],'No pinned third-party implementation/resources or actual comparison is available.')
    # Random rational checks and optional independent-library diagnostics.
    rng=random.Random(20260926)
    for i in range(20):
        a=[[rng.randint(-8,8) for x in range(4)] for y in range(3)]
        suite.case(f'rational_variance_offset_invariance_{i}',['FIL-20A'],'ExactRational',lambda a=a:(S.local_moments(a,3,3,(1,1))[1],S.local_moments([[v+2**40 for v in row] for row in a],3,3,(1,1))[1]),check=lambda r:equal(r[0],r[1]))
    if include_diagnostics:
        try:
            import numpy as np
            from scipy.fft import dctn
            for kind,n in [('I',1),('II',2),('III',3),('IV',4)]:
                for norm in ('backward','ortho'):
                    a=[[1.0,-2.0,3.0],[4.0,.5,-1.0]]
                    def compare(kind=kind,n=n,norm=norm):
                        ref=np.asarray(F.dct2(a,kind=kind,norm=norm));other=dctn(np.asarray(a),type=n,norm=norm)
                        return {'max_abs_difference':float(np.max(np.abs(ref-other))),'library':'scipy.fft.dctn','not_strict_proof':True}
                    suite.case(f'scipy_dct_diagnostic_{kind}_{norm}',['FRQ-09A'],'LibraryDiagnostic',compare,check=lambda r:require(r['max_abs_difference']<1e-12))
        except ImportError:
            suite.cases.append({'case_id':'scipy_optional_diagnostic','members':[],'proof_level':'LibraryDiagnostic','status':'skipped','reason':'NumPy/SciPy not installed','seconds':0})
    return suite
