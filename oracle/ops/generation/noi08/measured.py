from __future__ import annotations
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.measured import _mp
from oracle.ops.generation.common.measured import mp


def poisson_cdf(lam,k,dps=80):
    lam=frac(lam)
    if lam<0 or k<0:return mp.mpf(0)
    with mp.workdps(dps):
        if not lam:return mp.mpf(1)
        p=mp.exp(-_mp(lam));total=p
        for j in range(1,k+1):p*=_mp(lam)/j;total+=p
        return +total


def poisson_quantile(lam,u,dps=80,max_count=2000000):
    lam,u=frac(lam),frac(u)
    if lam<0 or not 0<u<1:raise ValueError('Poisson domain')
    if lam>1000:raise NotImplementedError('measured recurrence intentionally limited to lambda<=1000; large-lambda certificate is a gate')
    if not lam:return 0
    with mp.workdps(dps):
        rate=_mp(lam);target=_mp(u);p=mp.exp(-rate);cdf=p;k=0
        while cdf<target:
            k+=1
            if k>max_count:raise ArithmeticError('NotConverged')
            p*=rate/k;cdf+=p
        return k
