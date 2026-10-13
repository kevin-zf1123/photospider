from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.rng import _integer
from oracle.ops.generation.noi01.rng import block


# Fixed numerical permutation from the author reference (S08). This is data,
# not copied Java implementation; weighted polynomial arithmetic below is new.
PERM_2002=(
151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,
88,237,149,56,87,174,20,125,136,171,168,68,175,74,165,71,134,139,48,27,166,
77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,
102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,200,196,
135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,250,124,123,
5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,
223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,
129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,228,
251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,107,
49,192,214,31,181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,
138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180)


def fade(t):t=frac(t);return t*t*t*(t*(6*t-15)+10)


def _perlin_gradient(h,x,y,z):
    h&=15;u=x if h<8 else y
    v=y if h<4 else (x if h in (12,14) else z)
    return (-u if h&1 else u)+(-v if h&2 else v)


def perlin2002_fraction(coordinates):
    if len(coordinates)!=3:raise ValueError('3D coordinates')
    q=list(map(frac,coordinates));cell=[v//1 for v in q];f=[v-i for v,i in zip(q,cell)]
    weights=[(1-fade(t),fade(t)) for t in f]
    def p(i):return PERM_2002[i%256]
    total=F(0)
    for dx in (0,1):
        for dy in (0,1):
            for dz in (0,1):
                h=p(p(p(cell[0]+dx)+cell[1]+dy)+cell[2]+dz)
                g=_perlin_gradient(h,f[0]-dx,f[1]-dy,f[2]-dz)
                total+=weights[0][dx]*weights[1][dy]*weights[2][dz]*g
    return total


GRADIENTS_2D=((1,0),(-1,0),(0,1),(0,-1),(1,1),(-1,1),(1,-1),(-1,-1))
def gradient2d_fraction(coordinates,seed=0,stream=0,frame=0,period=(0,0)):
    if len(coordinates)!=2 or len(period)!=2:raise ValueError('2D coordinate/period')
    q=list(map(frac,coordinates));cell=[v//1 for v in q];f=[v-i for v,i in zip(q,cell)]
    for v in period:_integer(v,0,(1<<31)-1,'period')
    weights=[(1-fade(t),fade(t)) for t in f];total=F(0)
    for dx in (0,1):
        for dy in (0,1):
            ix,iy=cell[0]+dx,cell[1]+dy
            _integer(ix,-(1<<31),(1<<31)-1,'lattice x');_integer(iy,-(1<<31),(1<<31)-1,'lattice y')
            if period[0]:ix%=period[0]
            if period[1]:iy%=period[1]
            b=block(ix,iy,domain=6,seed=seed,stream=stream,frame=frame)
            g=GRADIENTS_2D[b[0]&7]
            dot=g[0]*(f[0]-dx)+g[1]*(f[1]-dy)
            total+=weights[0][dx]*weights[1][dy]*dot
    return total
