from __future__ import annotations
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.measured import _mp
from oracle.ops.generation.common.measured import mp


def star_vertices(center,outer_radius,inner_radius,n,rotation=0,dps=80):
    if not isinstance(n,int) or not 3<=n<=4096 or not 0<frac(inner_radius)<=frac(outer_radius):raise ValueError('star parameters')
    with mp.workdps(dps):
        out=[]
        for j in range(2*n):
            r=_mp(outer_radius if j%2==0 else inner_radius);a=_mp(rotation)+j*mp.pi/n
            out.append((float(_mp(center[0])+r*mp.cos(a)),float(_mp(center[1])+r*mp.sin(a))))
        return out


def ellipse_coverage(center,radii,pixel=(0,0),dps=80):
    if min(map(frac,radii))<=0:raise ValueError('positive radii')
    with mp.workdps(dps):
        cx,cy=map(_mp,center);rx,ry=map(_mp,radii);x0,y0=map(_mp,pixel);x1,y1=x0+1,y0+1
        left,right=max(x0,cx-rx),min(x1,cx+rx)
        if left>=right:return mp.mpf(0)
        xs={left,right}
        for y in (y0,y1):
            u=(y-cy)/ry
            if abs(u)<=1:
                dx=rx*mp.sqrt(max(mp.mpf(0),1-u*u))
                for x in (cx-dx,cx+dx):
                    if left<x<right:xs.add(x)
        def f(x):
            dy=ry*mp.sqrt(max(mp.mpf(0),1-((x-cx)/rx)**2))
            return max(mp.mpf(0),min(y1,cy+dy)-max(y0,cy-dy))
        xx=sorted(xs)
        return +sum(mp.quad(f,[a,b]) for a,b in zip(xx,xx[1:]))


def disk_area(center,radius,pixel=(0,0),dps=80):
    r=frac(radius)
    if r<0:raise ValueError('radius')
    return mp.mpf(0) if not r else ellipse_coverage(center,(r,r),pixel,dps)


def ellipse_distance(p,center,radii,dps=80):
    if min(map(frac,radii))<=0:raise ValueError('positive radii')
    with mp.workdps(dps):
        qx,qy=[abs(_mp(a)-_mp(b)) for a,b in zip(p,center)];rx,ry=map(_mp,radii)
        if qx==0 and qy==0:return -min(rx,ry)
        def ds(t):return (rx*mp.cos(t)-qx)**2+(ry*mp.sin(t)-qy)**2
        def dg(t):return (ry*ry-rx*rx)*mp.sin(t)*mp.cos(t)+rx*qx*mp.sin(t)-ry*qy*mp.cos(t)
        samples=[j*mp.pi/128 for j in range(65)];roots=[mp.mpf(0),mp.pi/2]
        for a,b in zip(samples,samples[1:]):
            da,db=dg(a),dg(b)
            if da==0:roots.append(a)
            if da*db<0:
                # Bisection is a reliable measured candidate finder; it is not
                # a complete exact-root isolation proof for arbitrary ellipses.
                for _ in range(dps*4+20):
                    mid=(a+b)/2;dm=dg(mid)
                    if da*dm<=0:b=mid
                    else:a=mid;da=dm
                roots.append((a+b)/2)
        d=mp.sqrt(min(ds(t) for t in roots))
        return +(-d if (qx/rx)**2+(qy/ry)**2<1 else d)
