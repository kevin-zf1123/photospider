from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.noi04.rng import gradient2d_fraction
from oracle.ops.generation.noi04.rng import perlin2002_fraction


def fractal_fraction(coordinates,kind='fbm',basis='gradient2d_philox',octaves=4,
                     frequency=1,lacunarity=2,gain=F(1,2),normalization='amplitude_sum',
                     footprint=None,**kw):
    q=tuple(map(frac,coordinates));freq,lac,gain=map(frac,(frequency,lacunarity,gain))
    if not 1<=octaves<=32 or freq<=0 or lac<=1 or not 0<=gain<=1:raise ValueError('fractal parameters')
    if kind not in ('fbm','turbulence','ridged'):raise ValueError('fractal kind')
    if basis not in ('gradient2d_philox','perlin2002_3d'):raise ValueError('basis')
    if normalization not in ('none','amplitude_sum'):raise ValueError('normalization')
    footprint=None if footprint is None else frac(footprint)
    if footprint is not None and (footprint<0 or kind!='fbm'):raise ValueError('footprint requires fbm')
    total=F(0);den=F(0);amp=F(1)
    for i in range(octaves):
        den+=amp
        weight=amp*(max(F(0),min(F(1),2-2*footprint*freq)) if footprint is not None else 1)
        if weight:
            at=[v*freq for v in q]
            n=gradient2d_fraction(at,**kw) if basis=='gradient2d_philox' else perlin2002_fraction(at)
            term=n if kind=='fbm' else (abs(n) if kind=='turbulence' else (1-abs(n)/2)**2)
            total+=weight*term
        freq*=lac;amp*=gain
    return total/(den if normalization=='amplitude_sum' else 1)
