from __future__ import annotations
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.measured import _mp
from oracle.ops.generation.common.measured import mp
from oracle.ops.generation.noi03.exact import correlate_kernel


def l2_kernel(source,kernel,boundary='zero',dps=80):
    numerator=correlate_kernel(source,kernel,boundary,'none')
    denom2=sum(frac(v)**2 for row in kernel for v in row)
    if not denom2:raise ValueError('zero kernel')
    with mp.workdps(dps):
        denom=mp.sqrt(_mp(denom2));return [[+(_mp(v)/denom) for v in row] for row in numerator]
