from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.bounds import certify
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import zeros

def frequency_grid(height,width,dy=1,dx=1):
    dy,dx=frac(dy),frac(dx)
    if min(height,width)<1 or min(dy,dx)<=0:raise DomainError('frequency grid')
    return [[(Q(y if y<=height//2 else y-height)/(height*dy),Q(x if x<=width//2 else x-width)/(width*dx)) for x in range(width)] for y in range(height)]

def _low(c,r2,cutoff,kind,order):
    if kind=='ideal':return c.q(int(r2<=cutoff*cutoff))
    if kind=='gaussian':return c.q(-r2/(2*cutoff*cutoff)).exp()
    return 1/c.q(1+(r2/(cutoff*cutoff))**order).sqrt()

def radial_response(height,width,*,cutoff,kind='gaussian',high=False,order=1,dy=1,dx=1):
    fc=frac(cutoff)
    if fc<=0 or kind not in ('ideal','gaussian','butterworth') or not isinstance(order,int) or order<1:raise DomainError('radial response')
    grid=frequency_grid(height,width,dy,dx);out=zeros(height,width);cache={}
    for y in range(height):
        for x in range(width):
            fy,fx=grid[y][x];r2=fy*fy+fx*fx
            if r2 not in cache:
                cache[r2]=certify(lambda c:1-_low(c,r2,fc,kind,order) if high else _low(c,r2,fc,kind,order))
            out[y][x]=cache[r2]
    return out,zeros(height,width)

def band_response(height,width,*,cutoff_low,cutoff_high,kind='gaussian',reject=False,order=1,dy=1,dx=1):
    lo,hi=frac(cutoff_low),frac(cutoff_high)
    if not 0<lo<hi or kind not in ('ideal','gaussian','butterworth') or order<1:raise DomainError('band response')
    grid=frequency_grid(height,width,dy,dx);out=zeros(height,width)
    for y in range(height):
        for x in range(width):
            fy,fx=grid[y][x];r2=fy*fy+fx*fx
            if r2==0:out[y][x]=float(bool(reject));continue
            out[y][x]=certify(lambda c:1-(_low(c,r2,hi,kind,order)-_low(c,r2,lo,kind,order)) if reject else _low(c,r2,hi,kind,order)-_low(c,r2,lo,kind,order))
    return out,zeros(height,width)

def torus_difference(d,period):
    d=frac(d)%period
    return d-period if d>period/2 else d

def notch_response(height,width,*,centers,sigma_y,sigma_x,depth=1,dy=1,dx=1):
    sy,sx,dep,dy,dx=map(frac,(sigma_y,sigma_x,depth,dy,dx))
    if min(sy,sx,dy,dx)<=0 or not 0<=dep<=1 or not centers:raise DomainError('notch parameters')
    py,px=1/dy,1/dx;cs=[];seen=set()
    for cy,cx in centers:
        c=(torus_difference(frac(cy),py),torus_difference(frac(cx),px))
        inv=(torus_difference(-c[0],py),torus_difference(-c[1],px))
        if c in seen or inv in seen:raise DomainError('duplicate notch conjugacy orbit')
        seen|={c,inv};cs.append(c)
        if inv!=c:cs.append(inv)
    grid=frequency_grid(height,width,dy,dx);out=zeros(height,width)
    for y in range(height):
        for x in range(width):
            fy,fx=grid[y][x]
            ds=[-((torus_difference(fy-cy,py)/sy)**2+(torus_difference(fx-cx,px)/sx)**2)/2 for cy,cx in cs]
            if dep==0:out[y][x]=1.0;continue
            if dep==1 and any(v==0 for v in ds):out[y][x]=0.0;continue
            def expr(c):
                v=c.q(1)
                for d in ds:v=v*(1-dep*c.q(d).exp())
                return v
            out[y][x]=certify(expr)
    return out,zeros(height,width)
