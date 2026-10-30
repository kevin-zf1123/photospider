#!/usr/bin/env python3
"""Evaluate strict member references or check their authored analytic samples."""
import argparse
import inspect
import json
from fractions import Fraction
from catalog import REFERENCES,SAMPLES,EXPECTED
from reference.exact import rn,bits,F,OracleError,solve_exact


def rounded(value,dtype):
    if isinstance(value,dict):return {k:rounded(v,dtype) for k,v in value.items()}
    if isinstance(value,(list,tuple)):return [rounded(v,dtype) for v in value]
    return rn(value,dtype)


def comparable(value):
    if isinstance(value,dict):return {k:comparable(v) for k,v in value.items()}
    if isinstance(value,(list,tuple)):return [comparable(v) for v in value]
    return value


def check_properties():
    from reference import blend as b,keying as k
    assert b.blend_channel(0,1,'hard_mix_vivid')==0
    assert b.blend_channel(0,1,'hard_mix_sum')==1
    assert k.lab_chroma_key([float('nan'),0,0],[float('nan'),0,0],0,1)==0
    assert b.blend_channel(float('nan'),.25,'normal')==.25
    assert b.composite([1,0,0,.5],[float('nan')]*3+[.5],'source_in')==[1.,0.,0.,.25]
    assert b.nonseparable([.5,float('nan'),float('nan')],[float('nan'),.25,.75],'color')==[.5,.25,.75]
    for dtype in ('f32','f64'):
        assert bits(b.premul_mix([-.0,1,0,0],[1,0,0,1],0,dtype)[0],dtype)==bits(-.0,dtype)
        for a in [0,.25,.5,1]:
            for alpha_b in [0,.25,.5,1]:
                source=[1,0,0,a];background=[0,0,1,alpha_b]
                assert b.composite(source,background,'source_over',dtype)==b.composite(background,source,'destination_over',dtype)
                assert b.composite_blended(source,background,source[:3],dtype)==b.composite(source,background,'source_over',dtype)
        for policy,expected in [('preserve_gray',[.5,0,0]),('source_hue',[.5,0,1]),('zero_hue',[.5,1,0])]:
            for model in ('lab','oklab'):
                assert b.nonseparable([.5,0,0],[.25,0,1],'chroma',model=model,dtype=dtype,neutral_policy=policy)==expected
        whole={(x,y):b.dissolve([1,0,0,.5],[0,0,1,.75],seed=7,x=x,y=y,frame=3,dtype=dtype) for y in range(-2,3) for x in range(-3,4)}
        # Re-evaluate nonzero disjoint requests with the same global coordinates.
        for x,y in [(3,2),(-3,-2),(0,1),(-1,0)]:
            assert b.dissolve([1,0,0,.5],[0,0,1,.75],seed=7,x=x,y=y,frame=3,dtype=dtype)==whole[x,y]
    assert bits(rn(1+F(1,2**24),'f32'),'f32')==bits(1.,'f32')
    assert bits(rn(-F(1,2**150),'f32'),'f32')==0x80000000
    assert solve_exact([[2,-1],[-1,2]],[1,0])==[F(2,3),F(1,3)]
    for callback in [lambda:k.clean_plate_nearest([[[0,0,0]]],[[0]]),
                     lambda:k.temporal_median([[[[0,0,0]]]],[[[0]]]),
                     lambda:k.closed_form_matting([[[0,0,0]]],[[1]],.125)]:
        try:callback()
        except OracleError:pass
        else:raise AssertionError('missing-evidence reference should report no solution')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check',action='store_true')
    parser.add_argument('--dtype',choices=['f32','f64'],default='f64')
    parser.add_argument('--member')
    args=parser.parse_args()
    ids=[args.member] if args.member else sorted(REFERENCES)
    for ident in ids:
        fn=REFERENCES[ident];pos,kw=SAMPLES[ident]
        options=kw|({'dtype':args.dtype} if 'dtype' in inspect.signature(fn).parameters else {})
        result=fn(*pos,**options)
        if ident in EXPECTED:
            expected=rounded(EXPECTED[ident],args.dtype)
            if comparable(result)!=expected:raise AssertionError((ident,result,expected,args.dtype))
        if not args.check:print(json.dumps({'member':ident,'dtype':args.dtype,'value':result},default=lambda x:str(x) if isinstance(x,Fraction) else x))
    if args.check:
        check_properties()
        print(f'{len(ids)} {args.dtype} member samples and authored anchors passed; composition, neutral policies, coordinates, rounding and evidence checks passed.')


if __name__=='__main__':main()
