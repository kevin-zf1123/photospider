from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.measured import _mp
from oracle.ops.generation.common.measured import mp


def arc_evaluate(center,u,v,start,t,sweep=None,direction=None,dps=80):
    if not 0<=frac(t)<=1:raise ValueError('t')
    with mp.workdps(dps):
        if direction is not None:
            if direction not in (-1,1) or sweep is not None:raise ValueError('fullturn')
            sweep=direction*2*mp.pi
            if frac(t)==1:t=F(0)
        else:sweep=_mp(sweep)
        theta=_mp(start)+_mp(t)*sweep;c,s=mp.cos(theta),mp.sin(theta)
        pos=tuple(+(_mp(center[i])+_mp(u[i])*c+_mp(v[i])*s) for i in range(2))
        der=tuple(+(sweep*(-_mp(u[i])*s+_mp(v[i])*c)) for i in range(2))
        return pos,der
