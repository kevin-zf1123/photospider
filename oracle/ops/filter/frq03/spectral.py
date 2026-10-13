from __future__ import annotations
from oracle.ops.filter.common.core import shape

def shift(image,*,inverse=False):
    h,w=shape(image);sy=h//2 if inverse else (h+1)//2;sx=w//2 if inverse else (w+1)//2
    return [[image[(y+sy)%h][(x+sx)%w] for x in range(w)] for y in range(h)]
