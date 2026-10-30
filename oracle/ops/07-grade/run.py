#!/usr/bin/env python3
"""Print strict reference samples, or verify the reference's analytic anchors."""
import argparse
import json
import math
from catalog import REFERENCES, SAMPLES, EXPECTED
from reference.precision import F, bits, rn, round_fraction, Uncertified


def equal(actual, expected):
    if isinstance(expected,(tuple,list)):
        return isinstance(actual,(tuple,list)) and len(actual)==len(expected) and all(equal(a,b) for a,b in zip(actual,expected))
    return actual==expected


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check',action='store_true',help='verify authored anchors and reference rounding properties')
    parser.add_argument('--member',help='one ID such as GRD-11C')
    args=parser.parse_args()
    ids=[args.member] if args.member else sorted(REFERENCES)
    anchors=0
    for key in ids:
        positional,keywords=SAMPLES[key]
        result=REFERENCES[key](*positional,**keywords)
        if key in EXPECTED:
            if not equal(result,EXPECTED[key]):raise AssertionError((key,result,EXPECTED[key]))
            anchors+=1
        if not args.check:print(json.dumps({'member':key,'value':result},allow_nan=False))
    if args.check:
        # Reference arithmetic checks include direct binary32 tie/subnormal and overflow.
        assert bits(round_fraction(1+F(1,2**24),32),32)==bits(1.,32)
        assert bits(round_fraction(F(1,2**150),32),32)==0
        assert bits(round_fraction(-F(1,2**150),32),32)==0x80000000
        assert math.isinf(round_fraction(F(2)**1024,64))
        from reference import statistics as st, extended as e, color as col, spatial as s
        assert st.quantiles([[0,10]],[[.75,.25]],[.5],mode='weighted')==([0.],2,1.)
        assert st.histogram([[0,1]],[[.25,.75]],2,mode='weighted')==([.25,.75],2,1.)
        assert st.quantiles([[0]],[[0]],[.5])==([0.],0,0.)
        assert s.threshold_gaussian([[.5,.5]],0,0)==1
        assert e.auto_levels([[[2.,3.,4.]]],[[0]])==[[[2.,3.,4.]]]
        assert e.colorize([0,1],[[1,0,0],[0,0,1]],[1,1,1])==[0.,0.,1.]
        ctx=col.Context()
        for route in ('ratio','xyz_y','cielab_l','oklab_l'):
            rgb=[.25,.5,.75]
            assert ctx.replace(rgb,ctx.feature(rgb,route),route)==rgb
        print(f'{len(ids)} member samples evaluated; {anchors} authored anchors and rounding/selection/route checks passed.')


if __name__=='__main__':
    main()
