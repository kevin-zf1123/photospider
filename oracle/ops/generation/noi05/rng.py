from __future__ import annotations
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.exact import sqrt_fraction
from oracle.ops.generation.common.rng import _inside_map
from oracle.ops.generation.common.rng import distance2
from oracle.ops.generation.noi01.rng import block
from oracle.ops.generation.noi01.rng import halfopen53


def cellular_feature(ix,iy,**kw):
    b=block(ix,iy,domain=7,**kw)
    return _inside_map(halfopen53(b[0],b[1]),ix,ix+1),_inside_map(halfopen53(b[2],b[3]),iy,iy+1)


def cellular2d(p,max_search_rings=64,dtype='float64',**kw):
    p=tuple(map(frac,p));ix,iy=(v//1 for v in p)
    candidates=[]
    for ring in range(max_search_rings+1):
        for y in range(iy-ring,iy+ring+1):
            for x in range(ix-ring,ix+ring+1):
                if max(abs(x-ix),abs(y-iy))!=ring:continue
                point=cellular_feature(x,y,**kw)
                candidates.append((distance2(p,point),y,x,point))
        candidates.sort(key=lambda q:q[:3])
        if len(candidates)>=2:
            lower=min(p[0]-(ix-ring),(ix+ring+1)-p[0],p[1]-(iy-ring),(iy+ring+1)-p[1])
            if lower*lower>candidates[1][0]:
                a,b=candidates[:2]
                return {'f1':sqrt_fraction(a[0],dtype),'f2':sqrt_fraction(b[0],dtype),
                        'cell':(a[2],a[1]),'distance2':(a[0],b[0]),
                        'rings':ring,'unvisited_lower2':lower*lower}
    raise ArithmeticError('NotConverged: F2 search certificate absent')
