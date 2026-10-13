"""Optional mpmath diagnostic references. ALL results here are MEASURED.

Using 80 and 160 decimal digits and obtaining equal binary outputs is useful
cross-checking, NOT an interval proof or all-input correctly-rounded oracle.
No scipy/numpy random sequence defines any Photospider output.
"""

from __future__ import annotations
from oracle.ops.generation.common.exact import frac
try:
    import mpmath as mp
except ImportError as exc:
    raise ImportError('Measured diagnostics require mpmath; see requirements-oracle.txt') from exc


def _mp(x):
    if isinstance(x,mp.mpf):return x
    q=frac(x);return mp.mpf(q.numerator)/q.denominator


def _bezier(controls,t,derivative=False):
    cp=[tuple(map(_mp,p)) for p in controls]
    if derivative:
        d=len(cp)-1;cp=[tuple(d*(b[j]-a[j]) for j in range(2)) for a,b in zip(cp,cp[1:])]
    while len(cp)>1:cp=[tuple((1-t)*a[j]+t*b[j] for j in range(2)) for a,b in zip(cp,cp[1:])]
    return cp[0]


def precision_agreement(function,*args,**kwargs):
    a=function(*args,dps=80,**kwargs);b=function(*args,dps=160,**kwargs)
    def flatten(x):
        if isinstance(x,(tuple,list)):
            return [v for y in x for v in flatten(y)]
        return [x]
    aa,bb=flatten(a),flatten(b)
    return {'quality':'Measured_not_certified','dps':[80,160],
            'same_binary64':all(float(x)==float(y) for x,y in zip(aa,bb)),
            'values_160dps':[mp.nstr(v,45) for v in bb]}
