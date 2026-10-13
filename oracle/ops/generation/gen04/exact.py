from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac


def checker(p, cell, lo=0, hi=1, phase=(0,0)):
    cx,cy=map(frac,cell)
    if min(cx,cy)<=0: raise ValueError('positive cell size')
    ix=(frac(p[0])-frac(phase[0]))//cx
    iy=(frac(p[1])-frac(phase[1]))//cy
    return lo if (ix+iy)%2==0 else hi


def grid_pattern(p, cell, line_width, phase=(0,0), lo=0, hi=1):
    c=list(map(frac,cell));w=list(map(frac,line_width))
    if any(v<=0 for v in c) or any(not 0<=a<=b for a,b in zip(w,c)):
        raise ValueError('invalid grid')
    inside=any((frac(p[i])-frac(phase[i]))%c[i]<w[i] for i in range(2))
    return hi if inside else lo


def ramp(index, count, lo=0, hi=1):
    if count<1 or not 0<=index<count:raise ValueError('invalid ramp')
    return frac(lo) if count==1 else frac(lo)+(frac(hi)-frac(lo))*F(index,count-1)


def impulse(shape, index):
    if len(shape)!=len(index) or any(n<1 or i<0 or i>=n for n,i in zip(shape,index)):
        raise ValueError('invalid impulse')
    def at(query): return int(tuple(query)==tuple(index))
    return at


BARS=((1,1,1),(1,1,0),(0,1,1),(0,1,0),(1,0,1),(1,0,0),(0,0,1),(0,0,0))
def color_bar(x,width):
    if width<1 or not 0<=x<width:raise ValueError('invalid bar index')
    return BARS[min(7,(8*(2*x+1))//(2*width))]
ramp_pattern=ramp
rgb_bars=color_bar
