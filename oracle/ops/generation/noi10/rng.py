from __future__ import annotations
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.noi04.rng import gradient2d_fraction
from oracle.ops.generation.noi04.rng import perlin2002_fraction


def advected_fraction(p,velocity,time,**kw):
    t=frac(time)
    if kw.get('frame',0)!=0:raise ValueError('continuous base frame must be zero')
    return gradient2d_fraction([frac(a)-frac(b)*t for a,b in zip(p,velocity)],**kw)


def temporal_fraction(p,time,time_scale=1,z_offset=0):
    scale=frac(time_scale)
    if scale<=0:raise ValueError('positive time_scale')
    return perlin2002_fraction([frac(p[0]),frac(p[1]),frac(time)*scale+frac(z_offset)])
