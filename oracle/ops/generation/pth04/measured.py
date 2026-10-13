from __future__ import annotations
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.measured import _bezier
from oracle.ops.generation.common.measured import _mp
from oracle.ops.generation.common.measured import mp
from oracle.ops.generation.pth03.measured import bezier_length


def inverse_bezier_arc(controls,normalized_s,dps=80):
    s=frac(normalized_s)
    if not 0<=s<=1:raise ValueError('s')
    with mp.workdps(dps):
        L=bezier_length(controls,dps=dps)
        if not L:return mp.mpf(0),tuple(map(_mp,controls[0]))
        a,b=mp.mpf(0),mp.mpf(1);target=_mp(s)*L
        for _ in range(min(256,dps*3)):
            t=(a+b)/2
            # Internal integration accepts mpf directly rather than converting
            # it through a rounded binary64 query.
            def speed(u):return mp.sqrt(sum(v*v for v in _bezier(controls,u,True)))
            value=mp.quad(speed,[0,t])
            if value<target:a=t
            else:b=t
        t=(a+b)/2;return +t,tuple(+v for v in _bezier(controls,t))
