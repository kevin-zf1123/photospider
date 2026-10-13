from __future__ import annotations
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.measured import _mp
from oracle.ops.generation.common.measured import mp
from oracle.ops.generation.gen05.exact import quadratic_coefficients


def angular(p,center,offset_turns=0,dps=80):
    with mp.workdps(dps):
        x,y=[_mp(a)-_mp(b) for a,b in zip(p,center)]
        if x==0 and y==0:return mp.mpf(0)
        u=mp.atan2(y,x)/(2*mp.pi)-_mp(offset_turns)
        return +(u-mp.floor(u))


def two_circle(p,c0,r0,c1,r1,dps=80):
    if min(frac(r0),frac(r1))<0 or (tuple(c0)==tuple(c1) and frac(r0)==frac(r1)):raise ValueError('invalid circles')
    A,B,C=quadratic_coefficients(p,c0,r0,c1,r1)
    with mp.workdps(dps):
        if not A:
            if not B:
                s=frac(r1)-frac(r0)
                return +(-_mp(r0)/_mp(s)) if C==0 and s<0 else None
            roots=[-_mp(C)/_mp(B)]
        else:
            disc=B*B-4*A*C
            if disc<0:return None
            root=mp.sqrt(_mp(disc));roots=[(-_mp(B)-root)/(2*_mp(A)),(-_mp(B)+root)/(2*_mp(A))]
        roots=[t for t in roots if _mp(r0)+t*(_mp(r1)-_mp(r0))>=0]
        return +max(roots) if roots else None
