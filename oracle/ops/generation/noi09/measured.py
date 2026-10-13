from __future__ import annotations
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.measured import _mp
from oracle.ops.generation.common.measured import mp


def speckle(signal,uniforms,dps=80):
    if not 1<=len(uniforms)<=64 or any(not 0<frac(u)<1 for u in uniforms):raise ValueError('speckle uniforms')
    with mp.workdps(dps):return +(-_mp(signal)*sum(mp.log(_mp(u)) for u in uniforms)/len(uniforms))
