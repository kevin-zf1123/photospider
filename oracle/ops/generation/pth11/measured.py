from __future__ import annotations
from oracle.ops.generation.common.measured import _bezier
from oracle.ops.generation.common.measured import _mp
from oracle.ops.generation.common.measured import mp


def closest_bezier(p,controls,dps=80):
    """Multi-start stationary-point diagnostic; not a global-root certificate."""
    with mp.workdps(dps):
        pp=tuple(map(_mp,p));candidates=[mp.mpf(0),mp.mpf(1)]
        def stationary(t):
            b,d=_bezier(controls,t),_bezier(controls,t,True)
            return sum((b[i]-pp[i])*d[i] for i in range(2))
        for i in range(33):
            try:
                t=mp.findroot(stationary,(mp.mpf(i)/32,mp.mpf(i+1)/32),maxsteps=100)
                if abs(mp.im(t))<mp.eps and 0<=mp.re(t)<=1:candidates.append(mp.re(t))
            except (ValueError,ZeroDivisionError):pass
        def ds(t):return sum((a-b)**2 for a,b in zip(_bezier(controls,t),pp))
        t=min(candidates,key=lambda t:(ds(t),t));return +mp.sqrt(ds(t)),+t,tuple(+v for v in _bezier(controls,t))
