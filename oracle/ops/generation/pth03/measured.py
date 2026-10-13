from __future__ import annotations
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.measured import _bezier
from oracle.ops.generation.common.measured import _mp
from oracle.ops.generation.common.measured import mp


def arc_length(u,v,start=0,sweep=None,direction=None,dps=80):
    with mp.workdps(dps):
        sw=direction*2*mp.pi if direction is not None else _mp(sweep)
        ux,uy=map(_mp,u);vx,vy=map(_mp,v);start=_mp(start)
        def speed(t):
            th=start+t*sw;s,c=mp.sin(th),mp.cos(th)
            return abs(sw)*mp.sqrt((-ux*s+vx*c)**2+(-uy*s+vy*c)**2)
        return +mp.quad(speed,[0,mp.mpf('.25'),mp.mpf('.5'),mp.mpf('.75'),1])


def bezier_length(controls,t=1,dps=80):
    if not 0<=frac(t)<=1:raise ValueError('t')
    with mp.workdps(dps):
        end=_mp(t)
        def speed(u):
            d=_bezier(controls,u,True);return mp.sqrt(sum(v*v for v in d))
        return +mp.quad(speed,[end*j/4 for j in range(5)])


def bspline_length(controls,knots,degree,weights=None,dps=80):
    """Measured speed integration by nonzero knot spans; no ABI validation."""
    with mp.workdps(dps):
        cp=[tuple(map(_mp,p)) for p in controls];k=list(map(_mp,knots));n=len(cp)
        w=[mp.mpf(1)]*n if weights is None else list(map(_mp,weights))
        a,b=k[degree],k[n]
        if a>=b or degree<0:raise ValueError('spline domain')
        if degree==0:return mp.mpf(0)
        def speed(u):
            cache={}
            def N(i,d):
                if i<0 or i+d+1>=len(k):return mp.mpf(0)
                if (i,d) in cache:return cache[i,d]
                if d==0:z=mp.mpf(int(k[i]<=u<k[i+1]))
                else:
                    d0,d1=k[i+d]-k[i],k[i+d+1]-k[i+1]
                    z=((u-k[i])*N(i,d-1)/d0 if d0 else 0)+((k[i+d+1]-u)*N(i+1,d-1)/d1 if d1 else 0)
                cache[i,d]=z;return z
            ns=[N(i,degree) for i in range(n)];ds=[]
            for i in range(n):
                d0,d1=k[i+degree]-k[i],k[i+degree+1]-k[i+1]
                ds.append((degree*N(i,degree-1)/d0 if d0 else 0)-(degree*N(i+1,degree-1)/d1 if d1 else 0))
            den=sum(z*v for z,v in zip(ns,w));dd=sum(z*v for z,v in zip(ds,w))
            if not den:return mp.mpf(0) # integration endpoints are measure zero
            num=[sum(z*v*c[j] for z,v,c in zip(ns,w,cp)) for j in range(2)]
            dn=[sum(z*v*c[j] for z,v,c in zip(ds,w,cp)) for j in range(2)]
            return mp.sqrt(sum(((dn[j]*den-num[j]*dd)/(den*den))**2 for j in range(2)))
        kk=sorted({a,b,*[z for z in k if a<z<b]})
        return +sum(mp.quad(speed,[x,y]) for x,y in zip(kk,kk[1:]))
