from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.exact import round_fraction
import math


def grid_points(bounds,nx,ny):
    x0,y0,x1,y1=map(frac,bounds)
    if nx<1 or ny<1 or x1<=x0 or y1<=y0:raise ValueError('grid bounds')
    out=[]
    for j in range(ny):
        for i in range(nx):
            left=x0+(x1-x0)*F(i,nx);right=x0+(x1-x0)*F(i+1,nx)
            top=y0+(y1-y0)*F(j,ny);bottom=y0+(y1-y0)*F(j+1,ny)
            x=round_fraction((left+right)/2);y=round_fraction((top+bottom)/2)
            # Half-open mapping belongs to the original cell, not the whole canvas.
            if F(x)>=right:x=math.nextafter(round_fraction(right),-math.inf)
            if F(y)>=bottom:y=math.nextafter(round_fraction(bottom),-math.inf)
            if not (left<=F(x)<right and top<=F(y)<bottom):raise ValueError('no representable cell point')
            out.append((x,y))
    if len(set(out))!=len(out):raise ValueError('duplicate rounded grid points')
    return out
