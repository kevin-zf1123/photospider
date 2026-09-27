"""Optional mpmath diagnostic references. ALL results here are MEASURED.

Using 80 and 160 decimal digits and obtaining equal binary outputs is useful
cross-checking, NOT an interval proof or all-input correctly-rounded oracle.
No scipy/numpy random sequence defines any Photospider output.
"""
from __future__ import annotations
import math
from fractions import Fraction as F
try:
    import mpmath as mp
except ImportError as exc:
    raise ImportError('Measured diagnostics require mpmath; see requirements-oracle.txt') from exc
from exact import frac, quadratic_coefficients, correlate_kernel


def _mp(x):
    if isinstance(x,mp.mpf):return x
    q=frac(x);return mp.mpf(q.numerator)/q.denominator


def gaussian(mean,sigma,u,v,dps=80):
    mean,sigma,u,v=map(frac,(mean,sigma,u,v))
    if sigma<0 or not 0<u<1 or not 0<=v<1:raise ValueError('Gaussian domain')
    with mp.workdps(dps):
        return +(_mp(mean)+_mp(sigma)*mp.sqrt(-2*mp.log(_mp(u)))*mp.cos(2*mp.pi*_mp(v)))


def zone_plate(p,center,k,phase=0,lo=0,hi=1,dps=80):
    with mp.workdps(dps):
        r2=sum((_mp(a)-_mp(b))**2 for a,b in zip(p,center))
        return +(_mp(lo)+(_mp(hi)-_mp(lo))*(1+mp.cos(2*mp.pi*_mp(k)*r2+_mp(phase)))/2)


def siemens_star(p,center,spokes,phase=0,lo=0,hi=1,dps=80):
    if not isinstance(spokes,int) or spokes<1:raise ValueError('spokes')
    with mp.workdps(dps):
        x,y=[_mp(a)-_mp(b) for a,b in zip(p,center)]
        if x==0 and y==0:return +((_mp(lo)+_mp(hi))/2)
        return +(_mp(lo)+(_mp(hi)-_mp(lo))*(1+mp.cos(spokes*mp.atan2(y,x)+_mp(phase)))/2)


def angular(p,center,offset_turns=0,dps=80):
    with mp.workdps(dps):
        x,y=[_mp(a)-_mp(b) for a,b in zip(p,center)]
        if x==0 and y==0:return mp.mpf(0)
        u=mp.atan2(y,x)/(2*mp.pi)-_mp(offset_turns)
        return +(u-mp.floor(u))


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


def two_circle(p,c0,r0,c1,r1,dps=80):
    if min(frac(r0),frac(r1))<0 or (tuple(c0)==tuple(c1) and frac(r0)==frac(r1)):raise ValueError('invalid circles')
    A,B,C=quadratic_coefficients(p,c0,r0,c1,r1)
    with mp.workdps(dps):
        if not A:
            if not B:
                s=frac(r1)-frac(r0)
                return +(-_mp(r0)/_mp(s)) if C==0 and s<0 else None
            roots=[-_mp(C)/_mp(B)]
        else:
            disc=B*B-4*A*C
            if disc<0:return None
            root=mp.sqrt(_mp(disc));roots=[(-_mp(B)-root)/(2*_mp(A)),(-_mp(B)+root)/(2*_mp(A))]
        roots=[t for t in roots if _mp(r0)+t*(_mp(r1)-_mp(r0))>=0]
        return +max(roots) if roots else None


def arc_evaluate(center,u,v,start,t,sweep=None,direction=None,dps=80):
    if not 0<=frac(t)<=1:raise ValueError('t')
    with mp.workdps(dps):
        if direction is not None:
            if direction not in (-1,1) or sweep is not None:raise ValueError('fullturn')
            sweep=direction*2*mp.pi
            if frac(t)==1:t=F(0)
        else:sweep=_mp(sweep)
        theta=_mp(start)+_mp(t)*sweep;c,s=mp.cos(theta),mp.sin(theta)
        pos=tuple(+(_mp(center[i])+_mp(u[i])*c+_mp(v[i])*s) for i in range(2))
        der=tuple(+(sweep*(-_mp(u[i])*s+_mp(v[i])*c)) for i in range(2))
        return pos,der


def arc_length(u,v,start=0,sweep=None,direction=None,dps=80):
    with mp.workdps(dps):
        sw=direction*2*mp.pi if direction is not None else _mp(sweep)
        ux,uy=map(_mp,u);vx,vy=map(_mp,v);start=_mp(start)
        def speed(t):
            th=start+t*sw;s,c=mp.sin(th),mp.cos(th)
            return abs(sw)*mp.sqrt((-ux*s+vx*c)**2+(-uy*s+vy*c)**2)
        return +mp.quad(speed,[0,mp.mpf('.25'),mp.mpf('.5'),mp.mpf('.75'),1])


def _bezier(controls,t,derivative=False):
    cp=[tuple(map(_mp,p)) for p in controls]
    if derivative:
        d=len(cp)-1;cp=[tuple(d*(b[j]-a[j]) for j in range(2)) for a,b in zip(cp,cp[1:])]
    while len(cp)>1:cp=[tuple((1-t)*a[j]+t*b[j] for j in range(2)) for a,b in zip(cp,cp[1:])]
    return cp[0]


def bezier_length(controls,t=1,dps=80):
    if not 0<=frac(t)<=1:raise ValueError('t')
    with mp.workdps(dps):
        end=_mp(t)
        def speed(u):
            d=_bezier(controls,u,True);return mp.sqrt(sum(v*v for v in d))
        return +mp.quad(speed,[end*j/4 for j in range(5)])


def inverse_bezier_arc(controls,normalized_s,dps=80):
    s=frac(normalized_s)
    if not 0<=s<=1:raise ValueError('s')
    with mp.workdps(dps):
        L=bezier_length(controls,dps=dps)
        if not L:return mp.mpf(0),tuple(map(_mp,controls[0]))
        a,b=mp.mpf(0),mp.mpf(1);target=_mp(s)*L
        for _ in range(min(256,dps*3)):
            t=(a+b)/2
            # Internal integration accepts mpf directly rather than converting
            # it through a rounded binary64 query.
            def speed(u):return mp.sqrt(sum(v*v for v in _bezier(controls,u,True)))
            value=mp.quad(speed,[0,t])
            if value<target:a=t
            else:b=t
        t=(a+b)/2;return +t,tuple(+v for v in _bezier(controls,t))


def closest_bezier(p,controls,dps=80):
    """Multi-start stationary-point diagnostic; not a global-root certificate."""
    with mp.workdps(dps):
        pp=tuple(map(_mp,p));candidates=[mp.mpf(0),mp.mpf(1)]
        def stationary(t):
            b,d=_bezier(controls,t),_bezier(controls,t,True)
            return sum((b[i]-pp[i])*d[i] for i in range(2))
        for i in range(33):
            try:
                t=mp.findroot(stationary,(mp.mpf(i)/32,mp.mpf(i+1)/32),maxsteps=100)
                if abs(mp.im(t))<mp.eps and 0<=mp.re(t)<=1:candidates.append(mp.re(t))
            except (ValueError,ZeroDivisionError):pass
        def ds(t):return sum((a-b)**2 for a,b in zip(_bezier(controls,t),pp))
        t=min(candidates,key=lambda t:(ds(t),t));return +mp.sqrt(ds(t)),+t,tuple(+v for v in _bezier(controls,t))


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


def poisson_cdf(lam,k,dps=80):
    lam=frac(lam)
    if lam<0 or k<0:return mp.mpf(0)
    with mp.workdps(dps):
        if not lam:return mp.mpf(1)
        p=mp.exp(-_mp(lam));total=p
        for j in range(1,k+1):p*=_mp(lam)/j;total+=p
        return +total


def poisson_quantile(lam,u,dps=80,max_count=2000000):
    lam,u=frac(lam),frac(u)
    if lam<0 or not 0<u<1:raise ValueError('Poisson domain')
    if lam>1000:raise NotImplementedError('measured recurrence intentionally limited to lambda<=1000; large-lambda certificate is a gate')
    if not lam:return 0
    with mp.workdps(dps):
        rate=_mp(lam);target=_mp(u);p=mp.exp(-rate);cdf=p;k=0
        while cdf<target:
            k+=1
            if k>max_count:raise ArithmeticError('NotConverged')
            p*=rate/k;cdf+=p
        return k


def speckle(signal,uniforms,dps=80):
    if not 1<=len(uniforms)<=64 or any(not 0<frac(u)<1 for u in uniforms):raise ValueError('speckle uniforms')
    with mp.workdps(dps):return +(-_mp(signal)*sum(mp.log(_mp(u)) for u in uniforms)/len(uniforms))


def l2_kernel(source,kernel,boundary='zero',dps=80):
    numerator=correlate_kernel(source,kernel,boundary,'none')
    denom2=sum(frac(v)**2 for row in kernel for v in row)
    if not denom2:raise ValueError('zero kernel')
    with mp.workdps(dps):
        denom=mp.sqrt(_mp(denom2));return [[+(_mp(v)/denom) for v in row] for row in numerator]


def precision_agreement(function,*args,**kwargs):
    a=function(*args,dps=80,**kwargs);b=function(*args,dps=160,**kwargs)
    def flatten(x):
        if isinstance(x,(tuple,list)):
            return [v for y in x for v in flatten(y)]
        return [x]
    aa,bb=flatten(a),flatten(b)
    return {'quality':'Measured_not_certified','dps':[80,160],
            'same_binary64':all(float(x)==float(y) for x,y in zip(aa,bb)),
            'values_160dps':[mp.nstr(v,45) for v in bb]}
